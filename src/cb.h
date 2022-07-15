/*
 * cb.h  callback interface for network
 * 14-Dec-2020  chuck@ece.cmu.edu
 */

/*
 * this is a simple I/O callback interface.  it is limited to allowing
 * only one send and one recv operation to be active at any one time.
 * (true for a simple stop-and-wait network protocol like ping-ping.)
 * the initial send from client to server uses "parameters" to set up
 * the application.  the intent is to provide enough hooks that it
 * can be used with mercury RPCs, the mercury NA API, and low-level
 * transports like PSM.
 */

/*
 * make_cb: make a cb of a given type.  writes listening addr string
 * to laddrbuf if (listen && laddr != NULL).
 */
struct cb *make_cb(char *type, char *info, int listen,
                   char *laddrbuf, int *laddrbuflen);

/*
 * cb: transport independent API.   typically embedded within a
 * transport-dependent structure.
 */
struct cb {
    /* reserved part of user's buffer, included in send/recv byte count  */
    int unexpected_reserve;
    int expected_reserve;

    /* setup and shutdown calls */
    void (*start_network)(struct cb *cb);
    void (*stop_network)(struct cb *cb);
    void (*finalize)(struct cb *cb);

    /* memory allocator (often a frontend for malloc) */
    void *(*alloc)(struct cb *cb, int size, void **metadatap);
    void (*free)(struct cb *cb, void *buf, void *metadata);

    /* parameter buffer setup/parse calls */
    int (*make_cb_params)(struct cb *cb, char *buf, int size, char *spec);
           /* ret #bytes used for internal cb params */
    int (*handle_cb_params)(struct cb *cb, char *buf, int size);
           /* ret #bytes consumed internally by cb */

    /* I/O: post calls wait for any prev I/O to complete before starting */
    void (*post_recv_buf)(struct cb *cb, void *buf, void *bufmd,
                          int size, int expected);
    int (*wait_for_recv)(struct cb *cb);  /* ret nbytes received */

    /* send: if buf==NULL, just wait for current send to complete */
    void (*post_send_buf)(struct cb *cb, void *buf, void *bufmd,
                          int size, int expected, int mode);

/* send modes - for RPC-based I/O, msg based I/O can ignore the mode */
#define CBSM_REQUEST       0     /* sending a client request */
#define CBSM_REQUEST_1WAY  1     /* sending final 1 way client request */
#define CBSM_RESPONSE      2     /* sending server respose */
};

/*
 * make_cb prototypes for various plugins
 */
#ifdef LCB_MERCURY
extern struct cb *make_cb_hg(char *opts, char *info, int listen,
                              char *laddrbuf, int *laddrbuflen);
extern struct cb *make_cb_na(char *opts, char *info, int listen,
                              char *laddrbuf, int *laddrbuflen);
#endif
#ifdef LCB_PSM
extern struct cb *make_cb_psm(char *opts, char *info, int listen,
                              char *laddrbuf, int *laddrbuflen);
#endif
