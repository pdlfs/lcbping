/*
 * cb_hg.c  mercury callback functions
 * 16-Dec-2020  chuck@ece.cmu.edu
 */

#include <err.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mercury.h>
#include <mercury_proc.h>
#include <mercury_thread.h>
#include <mercury_thread_condition.h>
#include <mercury_thread_mutex.h>

#include "cb.h"

#ifdef LCB_HG_DLOG

#include <mercury_dlog.h>
#define HGCB_DLOG_NENTS 4096      /* circular dlog buffer size */
struct hg_dlog_entry hgcb_le[HGCB_DLOG_NENTS];
struct hg_dlog hgcb_dlog = HG_DLOG_INITIALIZER("hgcb", hgcb_le,
                                               HGCB_DLOG_NENTS, 0);
#define HGCB_DLOG(MSG,DATA) \
    hg_dlog_addlog(&hgcb_dlog, __FILE__, __LINE__, __func__, MSG, DATA)

#else

#define HGCB_DLOG(MSG,DATA)  /* disabled, do nothing */

#endif

/*
 * hgcb_rpc_payload: the payload in an RPC call and in the reply back
 */
struct hgcb_rpc_payload {
    char *payload;              /* malloc'd payload buffer */
    uint32_t payload_alloc_sz;  /* allocation size of payload buffer */
    uint32_t payload_len;       /* # bytes in payload (<= payload_alloc_sz) */
};

/*
 * hgcb: private mercury state
 */
static struct hgcb {
    struct cb ecb;          /* embedded struct: must be first */

    hg_class_t *hc;         /* mercury class */
    hg_context_t *hcx;      /* mercury context */
    hg_addr_t self;         /* my address */
    hg_addr_t remote;       /* remote address */
    hg_id_t rpcid;          /* ID for my RPC */

    hg_thread_mutex_t lock; /* lock for state */
    hg_thread_cond_t cond;  /* user may wait here */
    hg_thread_t thread;     /* network thread */
    int net_running;        /* network thread running? */
    int stop_network;       /* stop the network thread */

    hg_handle_t rh;         /* recv handle (we are RPC server) */
    int recv_busy;          /* recv op is busy */
    int need_recv;          /* app waiting for rcvreq to clear */
    struct hgcb_rpc_payload rpay;    /* recv payload */

    hg_handle_t sh;         /* send handle (we are RPC client) */
    int send_busy;          /* send op is busy */
    int need_send;          /* app waiting for sndreq to clear */
    struct hgcb_rpc_payload spay;    /* send payload */
    int oneway;             /* if we are in oneway mode */

} hst;

/*
 * hgcb_proc_payload: proc function for the hgcb_rpc_payload strucure.
 * user must preallocate the payload buffer, we do not malloc.
 */
static hg_return_t hgcb_proc_payload(hg_proc_t proc, void *data) {
    hg_return_t ret = HG_SUCCESS;
    hg_proc_op_t op = hg_proc_get_op(proc);
    struct hgcb_rpc_payload *pload = (struct hgcb_rpc_payload *) data;

    ret = hg_proc_hg_int32_t(proc, &pload->payload_len);
    if (ret != HG_SUCCESS)
        errx(1, "hgcb_proc_payload: len coding failed: %s", 
             HG_Error_to_string(ret));

    if (pload->payload_len > pload->payload_alloc_sz)
        errx(1, "hgcb_proc_payload: len coding failed %d %d",
             pload->payload_len, pload->payload_alloc_sz);

    if (op == HG_DECODE || op == HG_ENCODE) {
        ret = hg_proc_memcpy(proc, pload->payload, pload->payload_len);
        if (ret != HG_SUCCESS) {
            errx(1, "hgcb_proc_payload: payload memcpy failed: %s",
                 HG_Error_to_string(ret));
        }
    }
 
    return(ret);
}

/*
 * hgcb_rpchandler: called on the server when a new RPC comes in.
 */
static hg_return_t hgcb_rpchandler(hg_handle_t handle) {
    const struct hg_info *hgi;
    struct hgcb *hcb;
    hg_return_t rv;

    /* extract "hcb" using handle */
    hgi = HG_Get_info(handle);
    if (!hgi) errx(1, "rpchandler: bad hgi");
    hcb = (struct hgcb *) HG_Registered_data(hgi->hg_class, hgi->id);
    if (!hcb) errx(1, "rpchandler: bad hcb");
    
    hg_thread_mutex_lock(&hcb->lock);
    if (!hcb->recv_busy)
        errx(1, "hgcb_rpchandler: no posted recv buffer for input");
    if (hcb->rh)
        errx(1, "hgcb_rpchandler: unresponded handle present");

    rv = HG_Get_input(handle, &hcb->rpay);
    if (rv != HG_SUCCESS)
        errx(1, "hgcb_rpchandler: unpack input failed: %s",
             HG_Error_to_string(rv));

    /*
     * our custom built proc function doesn't allocate or free
     * any memory.  it just copies data between our preallocated
     * buffer and the handle.  so we don't need to "free" anything,
     * but HG_Get_input() also gains a reference to the handle,
     * so we need to call HG_Free_input() to drop that reference
     * (otherwise we'll leak handles).
     */
    rv = HG_Free_input(handle, &hcb->rpay);
    if (rv != HG_SUCCESS)
        errx(1, "hgcb_rpchandler: HG_Free_input failed: %s", 
             HG_Error_to_string(rv));

    hcb->rh = handle;   /* save for HG_Respond() call */

    hcb->recv_busy = 0;
    if (hcb->need_recv) {
        hcb->need_recv = 0;
        hg_thread_cond_signal(&hcb->cond);
    }

    hg_thread_mutex_unlock(&hcb->lock);

    return(HG_SUCCESS);
}

/*
 * hgcb_progthread: HG progress thread.  most of the work will be in
 * the callback funcions.
 */
static void *hgcb_progthread(void *arg) {
    struct hgcb *hcb = arg;
    hg_return_t ret;
    unsigned int actual;

    printf("HG progthread: now running\n");
    while (hcb->stop_network == 0) {

        do {
            ret = HG_Trigger(hcb->hcx, 0, 1, &actual);
        } while (ret == HG_SUCCESS && actual);

        /* recheck, trigger may set stop_network */
        if (hcb->stop_network == 0) {
            ret = HG_Progress(hcb->hcx, 100);
            if (ret != HG_SUCCESS && ret != HG_TIMEOUT)
                errx(1, "progthead: HG_Progress: %s", HG_Error_to_string(ret));
        }

    }
    printf("HG progthread: now quitting\n");

    return(NULL);
}

/*
 * hgcb_startnet: start mercury network service
 */
static void hgcb_startnet(struct cb *cb) {
    struct hgcb *hcb = (struct hgcb *)cb;

    hg_thread_mutex_lock(&hcb->lock);

    if (hcb->net_running) errx(1, "hg_startnet: already running");
    hcb->stop_network = 0;   /* to be safe */

    if (hg_thread_create(&hcb->thread, hgcb_progthread, hcb) < 0)
        errx(1, "thread created failed?");
    hcb->net_running = 1;

    hg_thread_mutex_unlock(&hcb->lock);
}


/*
 * hgcb_stopnet: stop mercury network thread
 */
static void hgcb_stopnet(struct cb *cb) {
    struct hgcb *hcb = (struct hgcb *)cb;

    hg_thread_mutex_lock(&hcb->lock);

    if (hcb->net_running == 0)
        errx(1, "hgcb_stopnet: but not running!");
    hcb->stop_network = 1;

    hg_thread_mutex_unlock(&hcb->lock);

    printf("HG waiting for network to stop\n");
    hg_thread_join(hcb->thread);
    hcb->net_running = 0;
    printf("HG network thread join complete\n");
}

/*
 * hgcb_finalize: finalize mercurh
 */
static void hgcb_finalize(struct cb *cb) {
    struct hgcb *hcb = (struct hgcb *)cb;
    hg_return_t rv;

    if (hcb->net_running)
        errx(1, "hgcb_finalize: network is running?");

    if (hcb->rh && (rv = HG_Destroy(hcb->rh)) != HG_SUCCESS)
        errx(1, "HG destroy of rh failed: %s", HG_Error_to_string(rv));
    if (hcb->sh && (rv = HG_Destroy(hcb->sh)) != HG_SUCCESS)
        errx(1, "HG destroy of sh failed: %s", HG_Error_to_string(rv));

    if (hcb->remote && (rv = HG_Addr_free(hcb->hc, hcb->remote)) != HG_SUCCESS)
        errx(1, "HG addr free of remote failed: %s", HG_Error_to_string(rv));

    if (hcb->self && (rv = HG_Addr_free(hcb->hc, hcb->self)) != HG_SUCCESS)
        errx(1, "HG addr free of self failed: %s", HG_Error_to_string(rv));
    
    if ((rv = HG_Context_destroy(hcb->hcx)) != HG_SUCCESS)
        errx(1, "hgcb_finalize: free context: %s", HG_Error_to_string(rv));

    if ((rv = HG_Finalize(hcb->hc)) != HG_SUCCESS)
        errx(1, "hgcb_finalize: finalize class: %s", HG_Error_to_string(rv));

#ifdef LCB_HG_DLOG
    if (getenv("HGCB_DLOG_DUMP")) {
        printf("hg_finalize: dumping dlog\n");
        hg_dlog_dump_file(&hgcb_dlog, "dlog_hg", 1, 1);
    }
#endif

    printf("hg_finalize: OK\n");
}

/*
 * hgcb_alloc: allocate data buffer for mercury
 */
static void *hgcb_alloc(struct cb *cb, int size, void **metadatap) {
    *metadatap = NULL;
    return(malloc(size));
}

/*
 * hgcb_free: free data buffer
 */
static void hgcb_free(struct cb *cb, void *buf, void *metadata) {
    free(buf);
}

/*
 * hgcb_make_params: make params for sending initial message to server.
 * returns number of bytes of buffer we consumed.
 */
static int hgcb_make_params(struct cb *cb, char *buf, int size, char *spec) {
    struct hgcb *hcb = (struct hgcb *)cb;
    hg_return_t rv;

    if (hcb->remote)
        errx(1, "hgcb_make_params: already connected");

    if ((rv = HG_Addr_lookup2(hcb->hc, spec, &hcb->remote)) != HG_SUCCESS)
        errx(1, "remotelookup failed: %s", HG_Error_to_string(rv));
    printf("HG_Addr_lookup(%s): OK -> %p\n", spec, hcb->remote);

    if ((rv = HG_Create(hcb->hcx, hcb->remote,
                        hcb->rpcid, &hcb->sh)) != HG_SUCCESS)
        errx(1, "send handle create: %s", HG_Error_to_string(rv));
    
    return(0);   /* we don't need to add any data */
}

/*
 * hgcb_handle_params: handle params from initial message to server.
 * returns number of bytes of buffer we consumed.
 */
static int hgcb_handle_params(struct cb *cb, char *buf, int size) {

    /* we'll establish hcb->remote on server in the recv callback */
    return(0);
}

/*
 * hgcb_post_rbuf: post recv buffer for recv to use
 */
static void hgcb_post_rbuf(struct cb *cb, void *buf, void *bufmd,
                           int size, int expected) {
    struct hgcb *hcb = (struct hgcb *)cb;

    HGCB_DLOG("BEG", NULL);
    hg_thread_mutex_lock(&hcb->lock);
    HGCB_DLOG("locked", NULL);
    while (hcb->recv_busy) {
        hcb->need_recv = 1;
        HGCB_DLOG("pre-condwait", NULL);
        hg_thread_cond_wait(&hcb->cond, &hcb->lock);
        HGCB_DLOG("post-condwait", NULL);
    }
    hcb->rpay.payload = buf;
    hcb->rpay.payload_alloc_sz = size;
    hcb->rpay.payload_len = 0;    /* to be safe */
    hcb->recv_busy = 1;
    HGCB_DLOG("preunlock", NULL);
    hg_thread_mutex_unlock(&hcb->lock);
    HGCB_DLOG("END", NULL);
}

/*
 * hgcb_wait_for_recv: wait for recv to complete.  ret #bytes recvd
 */
static int hgcb_wait_for_recv(struct cb *cb) {
    struct hgcb *hcb = (struct hgcb *)cb;
    int rv;

    HGCB_DLOG("BEG", NULL);
    hg_thread_mutex_lock(&hcb->lock);
    HGCB_DLOG("locked", NULL);
    while (hcb->recv_busy) {
        hcb->need_recv = 1;
        HGCB_DLOG("pre-condwait", NULL);
        hg_thread_cond_wait(&hcb->cond, &hcb->lock);
        HGCB_DLOG("post-condwait", NULL);
    }
    rv = hcb->rpay.payload_len;
    HGCB_DLOG("preunlock", NULL);
    hg_thread_mutex_unlock(&hcb->lock);
    HGCB_DLOG("END", NULL);

   return(rv);
}

/*
 * hgcb_forw_cb: forw callback (called from trigger).
 */
static hg_return_t hgcb_forw_cb(const struct hg_cb_info *callback_info) {
    struct hgcb *hcb = callback_info->arg;
    hg_return_t ret;
    int do_signal;
    HGCB_DLOG("BEG", NULL);

    if (callback_info->ret != HG_SUCCESS)
        errx(1, "forw callback got a failure: %s", 
             HG_Error_to_string(callback_info->ret));
    if (callback_info->type != HG_CB_FORWARD)
        errx(1, "hgcb_forw_cb wrong type: %d", callback_info->type);
    if (callback_info->info.forward.handle != hcb->sh)
        errx(1, "hgcb_forw_cb handle mismatch");

    hg_thread_mutex_lock(&hcb->lock);
    HGCB_DLOG("locked", NULL);

    if (!hcb->oneway) {

        if (!hcb->recv_busy)
            errx(1, "hgcb_forw_cb: no recv buffer waiting for reply");

        ret = HG_Get_output(hcb->sh, &hcb->rpay);
        if (ret != HG_SUCCESS)
            errx(1, "forw HG_Get_output failed: %s", HG_Error_to_string(ret));

        /*
         * our custom built proc function doesn't allocate or free
         * any memory.  it just copies data between our preallocated
         * buffer and the handle.  so we don't need to "free" anything,
         * but HG_Get_output() also gains a reference to the handle,
         * so we need to call HG_Free_output() to drop that reference
         * (otherwise we'll hang).
         */
        ret = HG_Free_output(hcb->sh, &hcb->rpay);
        if (ret != HG_SUCCESS)
            errx(1, "forw HG_Free_output failed: %s", HG_Error_to_string(ret));

        do_signal = (hcb->need_recv || hcb->need_send);

        hcb->recv_busy = 0;
        if (hcb->need_recv)
            hcb->need_recv = 0;

    } else {

        do_signal = hcb->need_send;
    }

    hcb->send_busy = 0;
    if (hcb->need_send)
        hcb->need_send = 0;

    if (do_signal) {
        HGCB_DLOG("pre-signal", NULL);
        hg_thread_cond_signal(&hcb->cond);
        HGCB_DLOG("post-signal", NULL);
    }

    HGCB_DLOG("preunlock", NULL);
    hg_thread_mutex_unlock(&hcb->lock);

    HGCB_DLOG("END", NULL);
    return(0);
}

/*
 * hgcb_send_buf_req: send a buffer as a request (HG_Forward() on client)
 */
static void hgcb_send_buf_req(struct hgcb *hcb, void *buf, void *bufmd,
                             int size, int mode) {
    hg_return_t rv;
    HGCB_DLOG("BEG", buf);

    if (hcb->sh == NULL)
        errx(1, "send before sending params (no handle created)");

    hg_thread_mutex_lock(&hcb->lock);
    HGCB_DLOG("locked", buf);

    while (hcb->send_busy) {
        hcb->need_send = 1;
        HGCB_DLOG("pre-condwait", buf);
        hg_thread_cond_wait(&hcb->cond, &hcb->lock);
        HGCB_DLOG("post-condwait", buf);
    }
    HGCB_DLOG("owner", buf);

    if (mode == CBSM_REQUEST && hcb->oneway)
        errx(1, "hgcb_send_buf_req: normal req, but already in oneway mode");
    if (mode == CBSM_REQUEST_1WAY && !hcb->oneway) {
        rv = HG_Registered_disable_response(hcb->hc, hcb->rpcid, HG_TRUE);
        if (rv != HG_SUCCESS)
            errx(1, "hgcb_send_buf_req: failed to set oneway: %s",
                 HG_Error_to_string(rv));
        hcb->oneway = 1;
    }

    if (!buf)
        goto done;

    hcb->spay.payload = buf;
    hcb->spay.payload_alloc_sz = size;
    hcb->spay.payload_len = size;
    
    HGCB_DLOG("pre-HG_Forward", buf);
    rv = HG_Forward(hcb->sh, hgcb_forw_cb, hcb, &hcb->spay);
    HGCB_DLOG("post-HG_Forward", buf);

    if (rv != HG_SUCCESS)
        errx(1, "send forw post error: %s", HG_Error_to_string(rv));
    hcb->send_busy = 1;


done:
    HGCB_DLOG("preunlock", NULL);
    hg_thread_mutex_unlock(&hcb->lock);
    HGCB_DLOG("END", buf);
}

/*
 * hgcb_resp_cb: forw callback (called from trigger).
 */
static hg_return_t hgcb_resp_cb(const struct hg_cb_info *callback_info) {
    struct hgcb *hcb = callback_info->arg;

    if (callback_info->ret != HG_SUCCESS)
        errx(1, "resp callback got a failure: %s",
             HG_Error_to_string(callback_info->ret));
    if (callback_info->type != HG_CB_RESPOND)
        errx(1, "hgcb_respw_cb wrong type: %d", callback_info->type);
    if (callback_info->info.respond.handle != hcb->rh)
        errx(1, "hgcb_resp_cb handle mismatch");

    hg_thread_mutex_lock(&hcb->lock);

    HG_Destroy(hcb->rh);
    hcb->rh = NULL;
    hcb->send_busy = 0;
    if (hcb->need_send) {
        hcb->need_send = 0;
        hg_thread_cond_signal(&hcb->cond);
    }

    hg_thread_mutex_unlock(&hcb->lock);

    return(0);
}
/*
 * hgcb_send_buf_resp: send a buffer as a response (HG_Respond() on srvr)
 */
static void hgcb_send_buf_resp(struct hgcb *hcb, void *buf, void *bufmd,
                              int size) {
    hg_return_t rv;
    HGCB_DLOG("BEG", buf);
    hg_thread_mutex_lock(&hcb->lock);

    while (hcb->send_busy) {
        hcb->need_send = 1;
        HGCB_DLOG("pre-condwait", buf);
        hg_thread_cond_wait(&hcb->cond, &hcb->lock);
        HGCB_DLOG("post-condwait", buf);
    }

    if (hcb->rh == NULL)
        errx(1, "hgcb_send_buf_resp: no reponder waiting?");

    hcb->spay.payload = buf;
    hcb->spay.payload_alloc_sz = size;
    hcb->spay.payload_len = size;

    HGCB_DLOG("pre-HG_Respond", buf);
    rv = HG_Respond(hcb->rh, hgcb_resp_cb, hcb, &hcb->spay);
    HGCB_DLOG("post-HG_Respond", buf);
    if (rv != HG_SUCCESS)
        errx(1, "HG_Respond failed: %s", HG_Error_to_string(rv));

    hcb->send_busy = 1;

    hg_thread_mutex_unlock(&hcb->lock);
    HGCB_DLOG("END", buf);
}

/*
 * hgcb_send_buf: send a buffer (if NULL, just wait for send to free).
 */
static void hgcb_send_buf(struct cb *cb, void *buf, void *bufmd,
                          int size, int expected, int mode) {
    struct hgcb *hcb = (struct hgcb *)cb;

    if (mode == CBSM_REQUEST || mode == CBSM_REQUEST_1WAY)
        hgcb_send_buf_req(hcb, buf, bufmd, size, mode);
    else if (mode == CBSM_RESPONSE)
        hgcb_send_buf_resp(hcb, buf, bufmd, size);
    else
        errx(1, "hgcb_send_buf: bad mode?");
}

/*
 * cb_hg: all the functions collected in a struct
 */
static struct cb cb_hg = {
    0,
    0,
    hgcb_startnet,
    hgcb_stopnet,
    hgcb_finalize,
    hgcb_alloc,
    hgcb_free,
    hgcb_make_params,
    hgcb_handle_params,
    hgcb_post_rbuf,
    hgcb_wait_for_recv,
    hgcb_send_buf,
};

/*
 * make_cb_hg: make a mercury cb structure
 */
struct cb *make_cb_hg(char *opts, char *info, int listen,
                      char *laddrbuf, int *laddrbuflen) {
    struct hg_init_info hg_init_info = HG_INIT_INFO_INITIALIZER;
    hg_return_t rv;
    hg_size_t as;

    HGCB_DLOG("BEG", NULL);
    memset(&hst, 0, sizeof(hst));
    hst.ecb = cb_hg;    /* structure copy */
    hg_thread_mutex_init(&hst.lock);
    hg_thread_cond_init(&hst.cond);

    if (listen)
        HG_Cleanup();

    if (strstr(opts, "na_no_block") != NULL) {
        printf("setting NA_NO_BLOCK option!\n");
        hg_init_info.na_init_info.progress_mode = NA_NO_BLOCK;
    }

    hst.hc = HG_Init_opt(info, listen, &hg_init_info);
    if (!hst.hc) errx(1, "HG_Init_opt(%s, %d) failed", info, listen);
    printf("HG_Init_opt(%s,%d): OK\n", info, listen);

    hst.hcx = HG_Context_create(hst.hc);
    if (!hst.hcx) errx(1, "HG_Context_create failed");
    printf("HG_Context_create: OK\n");

    if ((rv = HG_Addr_self(hst.hc, &hst.self)) != HG_SUCCESS)
        errx(1, "HG_Addr_self failed: %s", HG_Error_to_string(rv));

    if (listen && laddrbuf) {
        as = *laddrbuflen;
        if ((rv = HG_Addr_to_string(hst.hc, laddrbuf, &as,
                                    hst.self)) != HG_SUCCESS)
            errx(1, "HG_Addr_to_string failed: %s", HG_Error_to_string(rv));
        *laddrbuflen = strlen(laddrbuf);
        printf("HG: listening on %s\n", laddrbuf);
    }

    /* register our RPC */
    hst.rpcid = HG_Register_name(hst.hc, "cb_hg", hgcb_proc_payload,
                                 hgcb_proc_payload, hgcb_rpchandler);
    printf("make_cb_hg: registered rpc %" PRIx64 "\n", (uint64_t)hst.rpcid);

    /* point RPC back at us */
    if ((rv = HG_Register_data(hst.hc, hst.rpcid, &hst, NULL)) != HG_SUCCESS)
        errx(1, "unable to register hst as data: %s", HG_Error_to_string(rv));

    HGCB_DLOG("END", NULL);
    return(&hst.ecb);
}
