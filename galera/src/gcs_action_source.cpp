//
// Copyright (C) 2010-2014 Codership Oy <info@codership.com>
//

#include "replicator.hpp"
#include "gcs_action_source.hpp"
#include "trx_handle.hpp"

#include "gu_serialize.hpp"

#include "galera_info.hpp"

#include <cassert>

// Exception-safe way to release action pointer when it goes out
// of scope
class Release
{
public:
    Release(struct gcs_action& act, gcache::GCache& gcache)
        :
        act_(act),
        gcache_(gcache)
    {}

    ~Release()
    {
        switch (act_.type)
        {
        case GCS_ACT_TORDERED:
            break;
        case GCS_ACT_STATE_REQ:
            gcache_.free(const_cast<void*>(act_.buf));
            break;
        default:
            ::free(const_cast<void*>(act_.buf));
            break;
        }
    }

private:
    struct gcs_action& act_;
    gcache::GCache&    gcache_;
};


galera::GcsActionTrx::GcsActionTrx(TrxHandle::SlavePool&    pool,
                                   const struct gcs_action& act)
    :
    txp_(TrxHandle::New(pool), TrxHandleDeleter())
    // TODO: this dynamic allocation should be unnecessary
{
    gu_trace(txp_->unserialize(static_cast<const gu::byte_t*>(act.buf), act.size, 0));

    //trx_->append_write_set(buf + offset, act.size - offset);
    // moved to unserialize trx_->set_write_set_buffer(buf + offset, act.size - offset);
    txp_->set_received(act.buf, act.seqno_l, act.seqno_g);
    txp_->lock();
}


galera::GcsActionTrx::~GcsActionTrx()
{
    txp_->unlock();
}

void galera::GcsActionSource::dispatch(void* const              recv_ctx,
                                       const struct gcs_action& act,
                                       bool&                    exit_loop)
{
    assert(recv_ctx != 0);
    assert(act.buf != 0);
    assert(act.seqno_l > 0);

    switch (act.type)
    {
    case GCS_ACT_TORDERED:
    {
        assert(act.seqno_g > 0);
        assert(act.seqno_l != GCS_SEQNO_ILL);
        GcsActionTrx const trx(trx_pool_, act);
        gu_trace(replicator_.process_trx(recv_ctx, trx.txp()));
        exit_loop = trx.txp()->exit_loop(); // this is the end of trx lifespan
        break;
    }
    case GCS_ACT_COMMIT_CUT:
    {
        wsrep_seqno_t seq;
        gu::unserialize8(static_cast<const gu::byte_t*>(act.buf), act.size, 0,
                         seq);
        gu_trace(replicator_.process_commit_cut(seq, act.seqno_l));
        break;
    }
    case GCS_ACT_CONF:
    {
        gu_trace(replicator_.process_conf_change(recv_ctx, act));
        break;
    }
    case GCS_ACT_STATE_REQ:
        gu_trace(replicator_.process_state_req(recv_ctx, act.buf, act.size,
                                               act.seqno_l, act.seqno_g));
        break;
    case GCS_ACT_JOIN:
    {
        wsrep_seqno_t seq;
        gu::unserialize8(static_cast<const gu::byte_t*>(act.buf),
                         act.size, 0, seq);
        gu_trace(replicator_.process_join(seq, act.seqno_l));
        break;
    }
    case GCS_ACT_SYNC:
        gu_trace(replicator_.process_sync(act.seqno_l));
        break;
    default:
        gu_throw_fatal << "unrecognized action type: " << act.type;
    }
}


ssize_t galera::GcsActionSource::process(void* recv_ctx, bool& exit_loop)
{
    struct gcs_action act;

    ssize_t rc(gcs_.recv(act));

    if (gu_likely(rc > 0))
    {
        Release release(act, gcache_);
        ++received_;
        received_bytes_ += rc;
        gu_trace(dispatch(recv_ctx, act, exit_loop));
    }

    return rc;
}
