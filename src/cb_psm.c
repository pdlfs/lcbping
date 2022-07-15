/*
 * cb_psm.c  PSM callback functions
 * 14-Dec-2020  chuck@ece.cmu.edu
 */

#include <err.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include "cb.h"

#include <psm.h>
#include <psm_mq.h>

#define DEFAULT_UUID	"0FFF0FFF-0000-0000-0000-0FFF0FFF0FFF"

/*
 * psmcb: private PSM state
 */
static struct psmcb {
    struct cb ecb;          /* embedded struct: must be first */
    psm_ep_t ep;            /* endpoint */
    psm_mq_t mq;            /* matched queue */
    psm_epid_t epid;        /* my local address (uint64_t) */
    psm_epaddr_t epaddr;    /* my local address (void * version) */
    uint64_t tag0;          /* init tag for unexpected startup */

    psm_epid_t rem_epid;    /* remote epid */
    psm_epaddr_t remepaddr; /* remote epaddr */

    pthread_mutex_t lock;   /* lock for state */
    pthread_cond_t cond;    /* user may wait here */
    pthread_t thread;       /* network thread */
    int net_running;        /* network thread running? */
    int stop_network;       /* stop the network thread */
    psm_mq_req_t rcvreq;    /* currently running recv request */
    psm_mq_status_t rstat;  /* recv status */
    int need_recv;          /* app waiting for rcvreq to clear */
    psm_mq_req_t sndreq;    /* currently running send request */
    psm_mq_status_t sstat;  /* recv status */
    int need_send;          /* app waiting for sndreq to clear */

} pst;

/*
 * psmcb_progthread: psm progress thread
 */
static void *psmcb_progthread(void *arg) {
    struct psmcb *pcb = arg;
    psm_error_t err;
    psm_mq_req_t gotreq, gotreq0;
    psm_mq_status_t status;

    printf("PSM progthread: now running\n");
    while (pcb->stop_network == 0) {

        err = psm_mq_ipeek(pcb->mq, &gotreq, NULL);
        if (err == PSM_MQ_NO_COMPLETIONS) {
            sched_yield();
            continue;
        }
        if (err != PSM_OK) errx(1, "ipeekloop %s", psm_error_get_string(err));

        gotreq0 = gotreq;
        err = psm_mq_test(&gotreq0, &status);
        if (err != PSM_OK) errx(1, "mq_test %s", psm_error_get_string(err));

        pthread_mutex_lock(&pcb->lock);
        if (gotreq == pcb->rcvreq) {
            pcb->rstat = status;
            pcb->rcvreq = NULL;
            if (pcb->need_recv) {
                pcb->need_recv = 0;
                pthread_cond_signal(&pcb->cond);
            }
        } else if (gotreq == pcb->sndreq) {
            pcb->sstat = status;
            pcb->sndreq = NULL;
            if (pcb->need_send) {
                pcb->need_send = 0;
                pthread_cond_signal(&pcb->cond);
            }
        } else {
            errx(1, "ipeek/test: unknown req");
        }
        pthread_mutex_unlock(&pcb->lock);

    }
    printf("PSM progthread: now quitting\n");

    return(NULL);
}

/*
 * psmcb_startnet: start PSM network service
 */
static void psmcb_startnet(struct cb *cb) {
    struct psmcb *pcb = (struct psmcb *)cb;

    pthread_mutex_lock(&pcb->lock);

    if (pcb->net_running) errx(1, "psm_startnet: already running");
    pcb->stop_network = 0;   /* to be safe */

    if (pthread_create(&pcb->thread, NULL, psmcb_progthread, pcb) != 0)
        errx(1, "pthread created failed?");
    pcb->net_running = 1;

    pthread_mutex_unlock(&pcb->lock);
}

/*
 * psmcb_stopnet: stop PSM network thread
 */
static void psmcb_stopnet(struct cb *cb) {
    struct psmcb *pcb = (struct psmcb *)cb;

    pthread_mutex_lock(&pcb->lock);

    if (pcb->net_running == 0)
        errx(1, "psmcb_stopnet: but not running!");
    pcb->stop_network = 1;

    pthread_mutex_unlock(&pcb->lock);

    printf("PSM waiting for network to stop\n");
    pthread_join(pcb->thread, NULL);
    pcb->net_running = 0;
    printf("PSM network thread join complete\n");
}

/*
 * psmcb_finalize: finalize PSM
 */
static void psmcb_finalize(struct cb *cb) {
    struct psmcb *pcb = (struct psmcb *)cb;
    psm_error_t err;

    if (pcb->net_running)
        errx(1, "psmcb_finalize: network is running?");

    err = psm_mq_finalize(pcb->mq);
    if (err != PSM_OK)
        errx(1, "psm_mq_finalize: %s", psm_error_get_string(err));
    printf("psm_mq_finalize: OK\n");

    /* nanosecs */
    err = psm_ep_close(pcb->ep, PSM_EP_CLOSE_GRACEFUL, 10 * 1000000000LL);
    if (err != PSM_OK) errx(1, "psm_ep_close: %s", psm_error_get_string(err));
    printf("psm_ep_close: OK\n");

    err = psm_finalize();
    if (err != PSM_OK) errx(1, "psm_finalzie: %s", psm_error_get_string(err));
    printf("psm_finalize: OK\n");
}

/*
 * psmcb_alloc: allocate data buffer for psm
 */
static void *psmcb_alloc(struct cb *cb, int size, void **metadatap) {
    *metadatap = NULL;
    return(malloc(size));
}

/*
 * psmcb_free: free data buffer
 */
static void psmcb_free(struct cb *cb, void *buf, void *metadata) {
    free(buf);
}

/*
 * psmcb_make_params: make params for sending initial message to server.
 * returns number of bytes of buffer we consumed.
 */
static int psmcb_make_params(struct cb *cb, char *buf, int size, char *spec) {
    struct psmcb *pcb = (struct psmcb *)cb;
    psm_error_t err, err2;

    if (size < sizeof(pcb->epid))
        errx(1, "psmcb_make_params: buf too small");
    memcpy(buf, &pcb->epid, sizeof(pcb->epid));

    if (pcb->rem_epid)
        errx(1, "psmcb_make_params: already connected");
     
    if (sscanf(spec, "%" SCNx64, &pcb->rem_epid) != 1)
        errx(1, "psmcb_make_params: epid sscanf error: %s", spec);
    printf("you said remote was: %" PRIx64 "\n", pcb->rem_epid);

    err = psm_ep_connect(pcb->ep, 1, &pcb->rem_epid, NULL, &err2,
                         &pcb->remepaddr, 30*1e9);
    if (err != PSM_OK) errx(1, "connect %s", psm_error_get_string(err));
    printf("psm_ep_connect(%s): OK: %p\n", spec, pcb->remepaddr);

    return(sizeof(pcb->epid));
}

/*
 * psmcb_handle_params: handle params from initial message to server.
 * returns number of bytes of buffer we consumed.
 */
static int psmcb_handle_params(struct cb *cb, char *buf, int size) {
    struct psmcb *pcb = (struct psmcb *)cb;
    psm_error_t err, err2;
    
    if (size < sizeof(pcb->rem_epid))
        errx(1, "psmcb_handle_params: buf too small");
    memcpy(&pcb->rem_epid, buf, sizeof(pcb->rem_epid));

    err = psm_ep_connect(pcb->ep, 1, &pcb->rem_epid, NULL, &err2,
                         &pcb->remepaddr, 30*1e9);
    if (err != PSM_OK) errx(1, "connect %s", psm_error_get_string(err));
    printf("psm_ep_connect(%" PRIx64 "): OK: %p\n", pcb->rem_epid,
           pcb->remepaddr);

    return(sizeof(pcb->rem_epid));
}

/*
 * psmcb_post_rbuf: post recv buffer
 */
static void psmcb_post_rbuf(struct cb *cb, void *buf, void *bufmd,
                            int size, int expected) {
    struct psmcb *pcb = (struct psmcb *)cb;
    psm_error_t err;
    uint64_t tag, sel;

    if (expected) {
        tag = pcb->rem_epid;
        sel = (uint64_t) -1;
    } else {
        tag = pcb->tag0;
        sel = pcb->tag0;
    }

    pthread_mutex_lock(&pcb->lock);
    while (pcb->rcvreq != NULL) {
        pcb->need_recv = 1;
        pthread_cond_wait(&pcb->cond, &pcb->lock);
    }
    err = psm_mq_irecv(pcb->mq, tag, sel, 0/*flags*/, buf, size,
                       NULL, &pcb->rcvreq);
    if (err != PSM_OK) errx(1, "irecv %s", psm_error_get_string(err));
    pthread_mutex_unlock(&pcb->lock);
}

/*
 * psmcb_wait_for_recv: wait for recv to complete.  ret #bytes recvd
 */
static int psmcb_wait_for_recv(struct cb *cb) {
    struct psmcb *pcb = (struct psmcb *)cb;

    if (pcb->rcvreq) {    /* busy? */
        pthread_mutex_lock(&pcb->lock);
        while (pcb->rcvreq != NULL) {
            pcb->need_recv = 1;
            pthread_cond_wait(&pcb->cond, &pcb->lock);
        }
        pthread_mutex_unlock(&pcb->lock);
    }

   return(pcb->rstat.nbytes);
}

/*
 * psmcb_send_buf: send a buffer (if NULL, just wait for send to free)
 */
static void psmcb_send_buf(struct cb *cb, void *buf, void *bufmd,
                           int size, int expected, int mode) {
    struct psmcb *pcb = (struct psmcb *)cb;
    uint64_t tag;
    psm_error_t err;

    tag = (expected) ? pcb->epid : pcb->tag0;

    pthread_mutex_lock(&pcb->lock);
    while (pcb->sndreq != NULL) {
        pcb->need_send = 1;
        pthread_cond_wait(&pcb->cond, &pcb->lock);
    }
    if (buf) {
        err = psm_mq_isend(pcb->mq, pcb->remepaddr, 0/*flags*/, tag,
                           buf, size, NULL, &pcb->sndreq);
        if (err != PSM_OK) errx(1, "isend %s", psm_error_get_string(err));
    }
    pthread_mutex_unlock(&pcb->lock);
}

/*
 * cb_psm: all the functions collected in a struct
 */
static struct cb cb_psm = {
    0,
    0,
    psmcb_startnet,
    psmcb_stopnet,
    psmcb_finalize,
    psmcb_alloc,
    psmcb_free,
    psmcb_make_params,
    psmcb_handle_params,
    psmcb_post_rbuf,
    psmcb_wait_for_recv,
    psmcb_send_buf,
};

/*
 * make_cb_psm: make a PSM cb structure.  currently not using info string.
 */
struct cb *make_cb_psm(char *opts, char *info, int listen,
                       char *laddrbuf, int *laddrbuflen) {
    int ver_major, ver_minor, sz;
    psm_error_t err, err2;
    struct psm_ep_open_opts psmopts;
    uint8_t uuid[sizeof(DEFAULT_UUID)];

    memset(&pst, 0, sizeof(pst));
    memcpy(uuid, DEFAULT_UUID, sizeof(uuid));
    pst.ecb = cb_psm;    /* structure copy */
    pthread_mutex_init(&pst.lock, NULL);
    pthread_cond_init(&pst.cond, NULL);

    /* psm_init: init PSM lib */
    ver_major = PSM_VERNO_MAJOR;
    ver_minor = PSM_VERNO_MINOR;
    err = psm_init(&ver_major, &ver_minor);
    if (err != PSM_OK) errx(1, "psm_init %s", psm_error_get_string(err));
    printf("psm_init: OK, version is %d.%d\n", ver_major, ver_minor);

    /* psm_ep_open: open an endpoint */
    psm_ep_open_opts_get_defaults(&psmopts);
    err = psm_ep_open(uuid, &psmopts, &pst.ep, &pst.epid);
    if (err != PSM_OK) errx(1, "psm_ep_open: %s", psm_error_get_string(err));
    printf("psm_ep_open: OK, my-epid=%" PRIx64 "\n", pst.epid);

    /* psm_mq_init: init matched queue */
    err = psm_mq_init(pst.ep, PSM_MQ_ORDERMASK_ALL, NULL, 0, &pst.mq);
    if (err != PSM_OK) errx(1, "psm_mq_init: %s", psm_error_get_string(err));
    printf("psm_mq_init: OK\n");

    /* psm_ep_connect: convert epid to epaddr struct pointer */
    pst.epaddr = NULL;    /* just to be safe */
    err = psm_ep_connect(pst.ep, 1, &pst.epid, NULL, &err2,
                         &pst.epaddr, 10*1e9);
    if (err != PSM_OK) errx(1, "psm_ep_connect: %s", psm_error_get_string(err));
    printf("psm_ep_connect: OK: %s %p\n", psm_error_get_string(err),
           pst.epaddr);

    pst.tag0 = (uint64_t) 1 << 63;

    if (listen && laddrbuf) {
        sz = snprintf(laddrbuf, *laddrbuflen, "%" PRIx64, pst.epid);
        if (sz >= *laddrbuflen)
            errx(1, "make_cb_psm: laddrbuf too small (%d)", *laddrbuflen);
        *laddrbuflen = sz;
    }

    return(&pst.ecb);
}
