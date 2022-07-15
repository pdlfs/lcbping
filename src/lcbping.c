/*
 * lcbping.c  leveled callback ping application
 * 14-Nov-2020  chuck@ece.cmu.edu
 */

#include <err.h>       /* XXX: non-standardized API, but everyone has it */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/time.h>

#ifdef LCB_MPI
#include <mpi.h>

/*
 * mpi_atexit(): use this with atexit(3) to ensure that MPI_Finalize()
 * is called for cleanup when we exit.
 */
static void mpi_atexit(void) {
    MPI_Finalize();
}
#endif

#include "cb.h"

/*
 * parameter buffer size.  used to exchange params from client to
 * server at start of the run.  increase if plugins need more space.
 */
#define PBUFSZ 64

/*
 * address buffer size.  buffer is used to store listening address
 * string for a server (for cases where we need the server to send
 * its address to a client).
 */
#define ABUFSZ 512

/*
 * advance buffer pointer/resid.  returns new residual or -1 on error.
 */
static int advance(char **ptr, int resid, int jump) {
    if (jump > resid)
        return(-1);
    *ptr = *ptr + jump;
    return(resid - jump);
}

/*
 * main program
 */
int main(int argc, char **argv) {
    char *type, *info, *addrspec, serveraddr[ABUFSZ], *remote;
    int count, is_server, serveraddrlen, l, maxreserve, got, size_w_reserve;
    uint64_t size, seq, reply, zero, want;
    struct cb *mycb;
    FILE *af;
    struct stat st;
    /* buffer related vars */
    char *pbuf0, *pb, *sbuf0, *sb, *rbuf0, *rb;
    void *pbmd, *sbmd, *rbmd;
    int presid, pb_used, rresid;
    /* client stuff */
    struct timeval tstart, tend;
    double ds, de;

    if (argc < 2) {
        fprintf(stderr, "usage:\n");
        fprintf(stderr, "    client: %s type info count size addr-spec\n",
                *argv);
        fprintf(stderr, "    server: %s type info [addr-spec]\n", *argv);
#ifdef LCB_MPI
        fprintf(stderr, "       MPI: mpirun %s type info count size MPI\n",
                *argv);
#endif
        fprintf(stderr, "\n");
        fprintf(stderr, "  type=plugin type (hg, na, psm, etc.)\n");
        fprintf(stderr, "  info=additional plugin info\n");
        fprintf(stderr, "  count=# msgs to send\n");
        fprintf(stderr, "  size=message payload size in bytes\n");
        fprintf(stderr, "  addr-spec=on client: server address\n");
        fprintf(stderr, "                        or file with addr in it\n");
        fprintf(stderr, "  addr-spec=on server: filename to write srvr addr\n");
        fprintf(stderr, "\n");
        exit(1);
    }

    type = argv[1];
    info = argv[2];
    if (argc == 3 || argc == 4) {  /* server cmdline (with or w/o addr-spec) */
        count = -1;     /* client sets count, sends EOF when done */
        size = 0;       /* updated to real value when we recv client params */
        addrspec = (argc == 3) ? NULL : argv[3];
    } else if (argc == 6) {
        count = atoi(argv[3]);
        size = atoi(argv[4]);
        addrspec = argv[5];
    } else {
        errx(1, "invalid number of args (%d)", argc);
    }

    if (addrspec && strcmp(addrspec, "MPI") == 0) {
#ifdef LCB_MPI
        int mpi_rank, mpi_size;
        if (MPI_Init(&argc, &argv) != MPI_SUCCESS)
            errx(1, "MPI_Init failed");
        if (atexit(mpi_atexit) != 0) { /* call MPI_Finalize() at exit time */
            mpi_atexit();
            errx(1, "atexit failed?");
        }
        if (MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank) != MPI_SUCCESS)
            errx(1, "MPI_Comm_rank failed");
        if (MPI_Comm_size(MPI_COMM_WORLD, &mpi_size) != MPI_SUCCESS)
            errx(1, "MPI_Comm_size failed");
        if (mpi_size != 2)
            errx(1, "Error: mpi_size != 2.  #procs must be 2!\n");
        if (count == -1)
            errx(1, "must provide valid count on cmdline with MPI");
        if (mpi_rank == 0)
            count = -1;  /* edit args to make rank 0 the server proc */
#else
    errx(1, "not compiled with MPI support!");
#endif
    }

    is_server = (count == -1);
    remote = NULL;

    /* sanity check args */
    if (!is_server) {
        if (count < 1)
            errx(1, "bad count: %d", count);
        if (size < sizeof(size))  /* ensure room for sequence number */
            errx(1, "bad size %" PRId64 ", must be >= %d", size,
             (int)sizeof(size));
    }

    /* create communications endpoint */
    serveraddrlen = sizeof(serveraddr);
    if (is_server && addrspec) {
        mycb = make_cb(type, info, 1, serveraddr, &serveraddrlen);
    } else {
        mycb = make_cb(type, info, is_server, NULL, NULL);
        serveraddrlen = 0;
    }

    if (!mycb)
        errx(1, "make_cb(%s,%s, ...) failed", type, info);

    /* manage address exchange (if needed) */
    if (addrspec) {
        if (strcmp(addrspec, "MPI") == 0) {
#ifdef LCB_MPI
            int rv;
            MPI_Status ms;
            if (is_server) 
                rv = MPI_Send(serveraddr, serveraddrlen+1, MPI_BYTE, 1, 1,
                              MPI_COMM_WORLD); 
            else
                rv = MPI_Recv(serveraddr, sizeof(serveraddr), MPI_BYTE, 0, 1,
                              MPI_COMM_WORLD, &ms);
            if (rv != MPI_SUCCESS)
                errx(1, "MPI send/recv failed!");
            if (!is_server) {
                printf("%s: got server address from MPI\n", *argv);
                remote = serveraddr;
            }
#else
            errx(1, "this MPI case cannot happen");
#endif
        } else if (is_server) {

            /* server: writes its address to shared filesystem */
            af = fopen(addrspec, "w");
            if (!af)
                err(1, "fopen: write addr to %s failed", addrspec);
            if (fprintf(af, "%s\n", serveraddr) < 0 || fclose(af))
                err(1, "write addr to %s failed", addrspec);
            printf("%s: saved server address to %s\n", *argv, addrspec);

        } else if (stat(addrspec, &st) != -1) {

            /* client: addrspec is a file (we assume server wrote) */
            af = fopen(addrspec, "r");
            if (!af)
                err(1, "fopen: read addr from %s failed", addrspec);
            if (fgets(serveraddr, sizeof(serveraddr), af) == NULL)
                errx(1, "fgets of %s failed", addrspec);
            fclose(af);
            l = strlen(serveraddr);
            if (l == 0 || serveraddr[l-1] != '\n')
                errx(1, "address file format error!");  /* shouldn't happen */
            serveraddr[l-1] = 0;
            serveraddrlen = l - 1;
            printf("%s: got server address from %s\n", *argv, addrspec);
            remote = serveraddr;
        } else {

            /* client: addrspec is an address string */
            remote = addrspec;
        }
    }

    /* deal with possible internal cb headers */
    maxreserve = mycb->unexpected_reserve;
    if (mycb->expected_reserve > maxreserve)
        maxreserve = mycb->expected_reserve;
    printf("max reserve: %d\n", maxreserve);

    /* allocate parameter buffer */
    pbuf0 = mycb->alloc(mycb, PBUFSZ+maxreserve, &pbmd);
    if (!pbuf0) errx(1, "pb alloc fail");
    pb = pbuf0;

    /* start progress thread */
    mycb->start_network(mycb);

    if (!is_server) {

        /* client must setup the parameter buffer to send to server */
        presid = advance(&pb, PBUFSZ+maxreserve, mycb->unexpected_reserve);
        if (presid < 0)
            errx(1, "short param reserve message");
        got = mycb->make_cb_params(mycb, pb, presid, remote);
        presid = advance(&pb, presid, got);
        if (presid < sizeof(size))
            errx(1, "short param message");
        memcpy(pb, &size, sizeof(size));   /* append size to param buf */
        presid = advance(&pb, presid, sizeof(size));
        pb_used = pb - pbuf0;

    } else {

        /* server must post param buf for unexpected recv and wait for data */
        mycb->post_recv_buf(mycb, pbuf0, pbmd, PBUFSZ+maxreserve, 0);
        presid = mycb->wait_for_recv(mycb);
        /* check buffer has recv'd at least unexpected_reserve bytes */
        presid = advance(&pb, presid, mycb->unexpected_reserve);
        if (presid < 0)
            errx(1, "short param message");
        /* now parse param block */
        pb_used = mycb->handle_cb_params(mycb, pb, presid);
        presid = advance(&pb, presid, pb_used);
        if (presid < sizeof(size))
            errx(1, "params missing size info");
        memcpy(&size, pb, sizeof(size));  /* assume byte order match */
        if (size < sizeof(size))
            errx(1, "size too small - %" PRId64, size);
        printf("server: size = %" PRId64 "\n", size);
        /* now the server knows the size too */
    }

    /* now that both ends know the size, allocate send/recv bufs */
    sbuf0 = mycb->alloc(mycb, size+maxreserve, &sbmd);
    rbuf0 = mycb->alloc(mycb, size+maxreserve, &rbmd);
    if (!sbuf0 || !rbuf0) errx(1, "alloc fail");

    /* setup some common values */
    sb = sbuf0 + mycb->expected_reserve;
    rb = rbuf0 + mycb->expected_reserve;
    size_w_reserve = size + mycb->expected_reserve;

    /*
     * now we split into client and server code.  we'll link back up
     * in the shutdown code.
     */
    if (!is_server) {

        /* client main loop */
        printf("entering send loop\n");
        gettimeofday(&tstart, NULL);
        for (seq = 0 ; seq < count ; seq++) {
            mycb->post_recv_buf(mycb, rbuf0, rbmd, size_w_reserve, 1);
            if (seq == 0) {
                /* we always send the params in seq 0 */
                mycb->post_send_buf(mycb, pbuf0, pbmd, pb_used,
                                    0, CBSM_REQUEST);
            } else {
                memcpy(sb, &seq, sizeof(seq));  /* update seq# */
                mycb->post_send_buf(mycb, sbuf0, sbmd, size_w_reserve,
                                    1, CBSM_REQUEST);
            }
            got = mycb->wait_for_recv(mycb);
            if (got != size_w_reserve)
                errx(1, "reply size wrong: %d != %d", got, size_w_reserve);
            memcpy(&reply, rb, sizeof(reply));
            if (reply != seq)
                errx(1, "reply mismatch: %" PRId64 " != %" PRId64, reply, seq);
        }
        gettimeofday(&tend, NULL);

        /* client done!   send EOF by setting seq to 0... */
        zero = 0;
        memcpy(sb, &zero, sizeof(zero));
        mycb->post_send_buf(mycb, sbuf0, sbmd, size_w_reserve, 1,
                            CBSM_REQUEST_1WAY);

        /* wait above send to complete before stopping, no buffer here */
        mycb->post_send_buf(mycb, NULL, NULL, 0, 1, CBSM_REQUEST_1WAY);

    } else { /* is_server */

        /* already recv'd seq#0 (params).  next one will be seq# 1 */
        want = 1;
        mycb->post_recv_buf(mycb, rbuf0, rbmd, size_w_reserve, 1);

        memset(sbuf0, 0, size+maxreserve);    /* send reply to seq# 0 */
        mycb->post_send_buf(mycb, sbuf0, sbmd, size_w_reserve,
                            1, CBSM_RESPONSE);
        /* loop until we get EOF */
        while (1) {
            rresid = mycb->wait_for_recv(mycb);
            if (rresid != size_w_reserve)
                errx(1, "size mismatch: %d != %d", rresid, size_w_reserve);

            memcpy(&seq, rb, sizeof(seq));
            if (seq == 0) {
                printf("server: got EOF!\n");
                break;
            }

            if (seq != want)
                errx(1, "unexpected seq: %" PRId64 " != %" PRId64, seq, want);

            mycb->post_recv_buf(mycb, rbuf0, rbmd, size_w_reserve, 1);
            want++;

            memcpy(sb, &seq, sizeof(seq));
            mycb->post_send_buf(mycb, sbuf0, sbmd, size_w_reserve, 1,
                                CBSM_RESPONSE);
        }
    }

    /* shutdown/free memory */
    mycb->stop_network(mycb);

    mycb->free(mycb, pbuf0, pbmd);
    mycb->free(mycb, sbuf0, sbmd);
    mycb->free(mycb, rbuf0, rbmd);

    mycb->finalize(mycb);

    if (!is_server) {

        /* client prints the results */
        ds = tstart.tv_sec + (tstart.tv_usec / 1000000.0);
        de = tend.tv_sec + (tend.tv_usec / 1000000.0);
        printf("%s %s ran in %lf seconds\n", type, info, de-ds);
    }

    exit(0);
}
