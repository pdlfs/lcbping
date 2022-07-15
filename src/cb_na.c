/*
 * cb_na.c  NA callback functions
 * 15-Dec-2020  chuck@ece.cmu.edu
 */

#include <err.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <na.h>
#include <mercury_thread.h>
#include <mercury_thread_condition.h>
#include <mercury_thread_mutex.h>

#include "cb.h"

#define TAG_EXP     1         /* just use same tag for all expected msgs */
#define TAG_UNEXP   2         /* just use same tag for all expected msgs */

/*
 * nacb: private NA state
 */
static struct nacb {
    struct cb ecb;          /* embedded struct: must be first */

    na_class_t *nc;         /* NA class */
    na_context_t *ncx;      /* NA context */
    na_addr_t self;         /* my address */
    na_addr_t remote;       /* remote address */
    int maxsize;            /* max size we can send */

    hg_thread_mutex_t lock; /* lock for state */
    hg_thread_cond_t cond;  /* user may wait here */
    hg_thread_t thread;     /* network thread */
    int net_running;        /* network thread running? */
    int stop_network;       /* stop the network thread */

    na_op_id_t *rop;        /* recv op_id */
    int recv_busy;          /* recv op is busy */
    int need_recv;          /* app waiting for rcvreq to clear */
    int bufsize;            /* size of recv buffer */
    int nbytes;             /* how much we got */

    na_op_id_t *sop;        /* send op_id */
    int send_busy;          /* send op is busy */
    int need_send;          /* app waiting for sndreq to clear */

} nst;

/*
 * nacb_progthread: NA progress thread.  most of the work will be in
 * the callback funcions.
 */
static void *nacb_progthread(void *arg) {
    struct nacb *ncb = arg;
    na_return_t ret;
    int cbr;
    unsigned int actual;

    printf("NA progthread: now running\n");
    while (ncb->stop_network == 0) {

        do {
            ret = NA_Trigger(ncb->ncx, 0, 1, &cbr, &actual);
        } while (ret == NA_SUCCESS && actual);

        /* recheck, trigger may set stop_network */
        if (ncb->stop_network == 0) {
            NA_Progress(ncb->nc, ncb->ncx, 100);
        }

    }
    printf("NA progthread: now quitting\n");

    return(NULL);
}

/*
 * nacb_startnet: start NA network service
 */
static void nacb_startnet(struct cb *cb) {
    struct nacb *ncb = (struct nacb *)cb;

    hg_thread_mutex_lock(&ncb->lock);

    if (ncb->net_running) errx(1, "na_startnet: already running");
    ncb->stop_network = 0;   /* to be safe */

    if (hg_thread_create(&ncb->thread, nacb_progthread, ncb) < 0)
        errx(1, "thread created failed?");
    ncb->net_running = 1;

    hg_thread_mutex_unlock(&ncb->lock);
}


/*
 * nacb_stopnet: stop NA network thread
 */
static void nacb_stopnet(struct cb *cb) {
    struct nacb *ncb = (struct nacb *)cb;

    hg_thread_mutex_lock(&ncb->lock);

    if (ncb->net_running == 0)
        errx(1, "nacb_stopnet: but not running!");
    ncb->stop_network = 1;

    hg_thread_mutex_unlock(&ncb->lock);

    printf("NA waiting for network to stop\n");
    hg_thread_join(ncb->thread);
    ncb->net_running = 0;
    printf("NA network thread join complete\n");
}

/*
 * nacb_finalize: finalize NA
 */
static void nacb_finalize(struct cb *cb) {
    struct nacb *ncb = (struct nacb *)cb;


    if (ncb->net_running)
        errx(1, "nacb_finalize: network is running?");

    if (ncb->rop)
        NA_Op_destroy(ncb->nc, ncb->rop);
    if (ncb->sop)
        NA_Op_destroy(ncb->nc, ncb->sop);
    
    if (ncb->remote && NA_Addr_free(ncb->nc, ncb->remote) != NA_SUCCESS)
        errx(1, "nacb_finalize: free remote failed");

    if (NA_Addr_free(ncb->nc, ncb->self) != NA_SUCCESS)
        errx(1, "nacb_finalize: free self failed");

    if (NA_Context_destroy(ncb->nc, ncb->ncx) != NA_SUCCESS)
        errx(1, "nacb_finalize: free context failed");

    if (NA_Finalize(ncb->nc) != NA_SUCCESS)
        errx(1, "nacb_finalize: finalize class failed");

    printf("na_finalize: OK\n");
}

/*
 * nacb_alloc: allocate data buffer for NA
 */
static void *nacb_alloc(struct cb *cb, int size, void **metadatap) {
    struct nacb *ncb = (struct nacb *)cb;

    return(NA_Msg_buf_alloc(ncb->nc, size, metadatap));
}

/*
 * nacb_free: free data buffer
 */
static void nacb_free(struct cb *cb, void *buf, void *metadata) {
    struct nacb *ncb = (struct nacb *)cb;

    NA_Msg_buf_free(ncb->nc, buf, metadata);
}

/*
 * nacb_make_params: make params for sending initial message to server.
 * returns number of bytes of buffer we consumed.
 */
static int nacb_make_params(struct cb *cb, char *buf, int size, char *spec) {
    struct nacb *ncb = (struct nacb *)cb;

    if (ncb->remote)
        errx(1, "nacb_make_params: already connected");

    if (NA_Addr_lookup(ncb->nc, spec, &ncb->remote) != NA_SUCCESS)
        errx(1, "remotelookup failed");
    printf("NA_Addr_lookup(%s): OK -> %p\n", spec, ncb->remote);

    return(0);   /* we don't need to add any data */
}

/*
 * nacb_handle_params: handle params from initial message to server.
 * returns number of bytes of buffer we consumed.
 */
static int nacb_handle_params(struct cb *cb, char *buf, int size) {

    /* we'll establish ncb->remote on server in the recv callback */
    return(0);
}

/*
 * nacb_recv_cb: receive callback (called from trigger).
 */
static int nacb_recv_cb(const struct na_cb_info *callback_info) {
    struct nacb *ncb = callback_info->arg;
    int type = callback_info->type;

    if (callback_info->ret != NA_SUCCESS)
        errx(1, "recv callback got a failure? (%d)", callback_info->ret);

    hg_thread_mutex_lock(&ncb->lock);
    if (type == NA_CB_RECV_UNEXPECTED) {
        if (ncb->remote)
            errx(1, "recv callback got unexpected msg after startup");
        ncb->remote = callback_info->info.recv_unexpected.source;
        printf("recv-cb: new connection addr=%p\n", ncb->remote);
        ncb->nbytes = callback_info->info.recv_unexpected.actual_buf_size;
    }
    ncb->recv_busy = 0;
    if (ncb->need_recv) {
        ncb->need_recv = 0;
        hg_thread_cond_signal(&ncb->cond);
    }
    hg_thread_mutex_unlock(&ncb->lock);

    return(0);
}

/*
 * nacb_post_rbuf: post recv buffer
 */
static void nacb_post_rbuf(struct cb *cb, void *buf, void *bufmd,
                           int size, int expected) {
    struct nacb *ncb = (struct nacb *)cb;
    na_return_t rv;

    if (size > ncb->maxsize)
        errx(1, "recv size %d too large for this config (max=%d)", size,
             ncb->maxsize);

    hg_thread_mutex_lock(&ncb->lock);
    while (ncb->recv_busy) {
        ncb->need_recv = 1;
        hg_thread_cond_wait(&ncb->cond, &ncb->lock);
    }
    if (expected) {
        rv = NA_Msg_recv_expected(ncb->nc, ncb->ncx, nacb_recv_cb, ncb,
                                  buf, size, bufmd, ncb->remote, 0,
                                  TAG_EXP, ncb->rop);
    } else {
        rv = NA_Msg_recv_unexpected(ncb->nc, ncb->ncx, nacb_recv_cb, ncb,
                                    buf, size, bufmd, ncb->rop);
    }
    if (rv != NA_SUCCESS)
        errx(1, "recv post error");
    ncb->recv_busy = 1;
    ncb->bufsize = ncb->nbytes = size;
    hg_thread_mutex_unlock(&ncb->lock);
}

/*
 * nacb_wait_for_recv: wait for recv to complete.  ret #bytes recvd
 */
static int nacb_wait_for_recv(struct cb *cb) {
    struct nacb *ncb = (struct nacb *)cb;
    int rv;

    hg_thread_mutex_lock(&ncb->lock);
    while (ncb->recv_busy) {
        ncb->need_recv = 1;
        hg_thread_cond_wait(&ncb->cond, &ncb->lock);
    }
    rv = ncb->nbytes;
    hg_thread_mutex_unlock(&ncb->lock);

   return(rv);
}

/*
 * nacb_send_cb: send callback (called from trigger).
 */
static int nacb_send_cb(const struct na_cb_info *callback_info) {
    struct nacb *ncb = callback_info->arg;

    if (callback_info->ret != NA_SUCCESS)
        errx(1, "send callback got a failure?");

    hg_thread_mutex_lock(&ncb->lock);
    ncb->send_busy = 0;
    if (ncb->need_send) {
        ncb->need_send = 0;
        hg_thread_cond_signal(&ncb->cond);
    }
    hg_thread_mutex_unlock(&ncb->lock);

    return(0);
}

/*
 * nacb_send_buf: send a buffer (if NULL, just wait for send to free).
 * caller must reserve room for init_expected/init_unexpected at front
 * of the buffer for us to overwrite (if those headers are used).
 */
static void nacb_send_buf(struct cb *cb, void *buf, void *bufmd,
                          int size, int expected, int mode) {
    struct nacb *ncb = (struct nacb *)cb;
    na_return_t rv;

    if (size > ncb->maxsize)
        errx(1, "send size %d too large for this config (max=%d)", size,
             ncb->maxsize);

    hg_thread_mutex_lock(&ncb->lock);
    while (ncb->send_busy) {
        ncb->need_send = 1;
        hg_thread_cond_wait(&ncb->cond, &ncb->lock);
    }
    if (!buf)
        goto done;
    if (expected) {
        if (NA_Msg_init_expected(ncb->nc, buf, size) != NA_SUCCESS)
            errx(1, "nacb_send_buf: init expected failed!");
        rv = NA_Msg_send_expected(ncb->nc, ncb->ncx, nacb_send_cb, ncb,
                                  buf, size, bufmd, ncb->remote, 0,
                                  TAG_EXP, ncb->sop);
    } else {
        if (NA_Msg_init_unexpected(ncb->nc, buf, size) != NA_SUCCESS)
            errx(1, "nacb_send_buf: init unexpected failed!");
        rv = NA_Msg_send_unexpected(ncb->nc, ncb->ncx, nacb_send_cb, ncb,
                                    buf, size, bufmd, ncb->remote, 0,
                                    TAG_UNEXP, ncb->sop);
    }
    if (rv != NA_SUCCESS)
        errx(1, "send post error");
    ncb->send_busy = 1;


done:
    hg_thread_mutex_unlock(&ncb->lock);
}

/*
 * cb_na: all the functions collected in a struct
 */
static struct cb cb_na = {
    0,
    0,
    nacb_startnet,
    nacb_stopnet,
    nacb_finalize,
    nacb_alloc,
    nacb_free,
    nacb_make_params,
    nacb_handle_params,
    nacb_post_rbuf,
    nacb_wait_for_recv,
    nacb_send_buf,
};

/*
 * make_cb_na: make a NA cb structure
 */
struct cb *make_cb_na(char *opts, char *info, int listen,
                      char *laddrbuf, int *laddrbuflen) {
    struct na_init_info na_init_info = NA_INIT_INFO_INITIALIZER;
    size_t as;   /* na_size_t went away */
    int tmp;

    memset(&nst, 0, sizeof(nst));
    nst.ecb = cb_na;    /* structure copy */
    hg_thread_mutex_init(&nst.lock);
    hg_thread_cond_init(&nst.cond);

    if (listen)
        NA_Cleanup();

    if (strstr(opts, "na_no_block") != NULL) {
        printf("setting NA_NO_BLOCK option!\n");
        na_init_info.progress_mode = NA_NO_BLOCK;
    }

    nst.nc = NA_Initialize_opt(info, listen, &na_init_info);
    if (!nst.nc) errx(1, "NA_Initialize_opt(%s, %d) failed", info, listen);
    printf("NA_Initialize(%s,%d): OK\n", info, listen);

    nst.ncx = NA_Context_create(nst.nc);
    if (!nst.ncx) errx(1, "NA_Context_create failed");
    printf("NA_Context_create: OK\n");

    if (NA_Addr_self(nst.nc, &nst.self) != NA_SUCCESS)
        errx(1, "NA_Addr_self failed");

    if (listen && laddrbuf) {
        as = *laddrbuflen;
        if (NA_Addr_to_string(nst.nc, laddrbuf, &as, nst.self) != NA_SUCCESS)
            errx(1, "NA_Addr_to_string failed");
        *laddrbuflen = strlen(laddrbuf);
        printf("NA: listening on %s\n", laddrbuf);
    }

    nst.maxsize = NA_Msg_get_max_unexpected_size(nst.nc);
    tmp = NA_Msg_get_max_expected_size(nst.nc);
    printf("max msg sizes: unexpected=%d, expected=%d\n", nst.maxsize, tmp);
    if (tmp < nst.maxsize)
        nst.maxsize = tmp;   /* choose smaller value */
    if (nst.maxsize < 16)
        errx(1, "max size too small?");
    printf("size limit: %d bytes\n", nst.maxsize);

    nst.rop = NA_Op_create(nst.nc);
    nst.sop = NA_Op_create(nst.nc);
    if (!nst.rop || !nst.sop)
        errx(1, "make_cb_na: op_id alloc fail");

    nst.ecb.unexpected_reserve = NA_Msg_get_unexpected_header_size(nst.nc);
    nst.ecb.expected_reserve = NA_Msg_get_expected_header_size(nst.nc);

    printf("reserved header bytes: unexpected=%d, expected=%d\n",
           nst.ecb.unexpected_reserve, nst.ecb.expected_reserve);

    return(&nst.ecb);
}
