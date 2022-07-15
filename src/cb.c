/*
 * cb.c  callback interface for network
 * 14-Dec-2020  chuck@ece.cmu.edu
 */

#include <stdio.h>
#include <string.h>
#include "cb.h"

/*
 * make_cb: make a cb of a given type
 */
struct cb *make_cb(char *type, char *info, int listen, char *laddrbuf,
                   int *laddrbuflen) {
    char *tmp, *opts;
    int l;

    tmp = strchr(type, ',');
    if (tmp == NULL) {
        l = strlen(type);
        opts = type + l;     /* empty string */
    } else {
        l = tmp - type;
        opts = tmp + 1;      /* skip the comma */
    }

#ifdef LCB_MERCURY
    if (strncmp(type, "hg", l) == 0)
        return(make_cb_hg(opts, info, listen, laddrbuf, laddrbuflen));
    if (strncmp(type, "na", l) == 0)
        return(make_cb_na(opts, info, listen, laddrbuf, laddrbuflen));
#endif
#ifdef LCB_PSM
    if (strncmp(type, "psm", l) == 0)
        return(make_cb_psm(opts, info, listen, laddrbuf, laddrbuflen));
#endif

    return(NULL);
}
