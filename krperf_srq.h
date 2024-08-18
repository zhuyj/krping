#ifndef _KRPERF_SRQ_H
#define _KRPERF_SRQ_H

#include "krperf.h"

int krperf_alloc_srq(struct krping_cb *cb);
void krperf_free_srq(struct krping_cb *cb);
int krperf_ib_srq_rq_post_recv(struct krping_cb *cb, const struct ib_recv_wr **bad_wr);

static inline bool krperf_srq_valid(struct krping_cb *cb)
{
        if (cb != NULL && cb->use_srq && cb->srq != NULL)
                return true;

        return false;
}
#endif /* _KRPERF_SRQ_H */
