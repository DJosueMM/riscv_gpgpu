// barrier_arbiter.h - HLS-synthesizable global barrier/launch arbiter state
// and free functions
//
// Golden reference: GPGPUTop::simulationProcess()'s barrier_queue mechanics
// (models/systemc/src/top/top.cpp) - specifically the release condition
// `barrier_queue.size() == total_warps_` (top.cpp:157), global across every
// CU, ported bit-faithfully per docs/hls/interfaces.md SS10.6 (the barrier
// scope reversal - global, not block-scoped).
//
// One state instance, system-wide - counts arrivals across every CU instead
// of each CU deciding release locally (docs/hls/interfaces.md SS10.7).
// Deliberately decoupled from warp-slot bookkeeping (cu_dispatch_unit.h):
// nothing here inspects a WarpSlot directly. The top-level wiring
// (gpgpu_top.cpp's schedulerCore) is responsible for calling
// barrierOnEvent() and the matching recordResult() together, every time a
// warp_status_t comes back from compute_pipeline, and for calling
// releaseBarrierSlots() once barrierReleaseReady() is true, followed by
// barrierAcknowledgeRelease().
//
// docs/hls/interfaces.md SS16.6: rewritten from a BarrierArbiter class to a
// plain BarrierState struct plus free functions, the same treatment
// cu_dispatch_unit.h's warp-slot bookkeeping got - not because this state
// was ever observed to fail real DATAFLOW checking (it's four plain
// scalars, no arrays; every failure across 10 real csynth attempts named a
// specific ARRAY element, never a bare scalar), but for consistency with
// schedulerCore now owning all scheduling state as genuinely local
// variables, and to close out any remaining doubt rather than leave one
// piece of scheduler state on the old, failure-prone shape.
//
// Verification note on the release condition: `stalled_count_ ==
// total_warps_` only becomes true if EVERY warp in the kernel is currently
// stalled - if even one warp completes without ever reaching the barrier,
// stalled_count_ can never reach total_warps_ again (done_count_ absorbs
// it instead), so barrierReleaseReady() correctly never fires. This
// reproduces simulationProcess()'s own inherited requirement (docs/hls/
// interfaces.md SS10.7's "inherited assumption" note) - a kernel with
// non-uniform barrier participation hangs the golden model too, in
// software - as a direct consequence of the counter arithmetic, not a
// special case added here.

#ifndef RISCV_GPGPU_HLS_BARRIER_ARBITER_H
#define RISCV_GPGPU_HLS_BARRIER_ARBITER_H

#include <hls_stream.h>
#include "../common/hls_config.h"
#include "../common/hls_types.h"

namespace riscv_gpgpu_hls {

struct BarrierState {
    warp_id_t total_warps_   = 0;
    warp_id_t stalled_count_ = 0;
    warp_id_t done_count_    = 0;
    bool      launch_fault_  = false;
};

// Called once per kernel launch. Latches total_warps and performs the
// SS10.6 hazard mitigation: a kernel bigger than the hardware's declared
// resident capacity is an invalid launch, not a silent hang.
inline void barrierLaunch(BarrierState& b, warp_id_t total_warps) {
    b.total_warps_   = total_warps;
    b.stalled_count_ = 0;
    b.done_count_    = 0;
    b.launch_fault_  = (total_warps > warp_id_t(NUM_CUS * MAX_WARPS_PER_CU));
}

// Host-visible `status.fault` (docs/hls/interfaces.md SS2.5.6).
inline bool barrierLaunchFault(const BarrierState& b) { return b.launch_fault_; }

// Call once per {slot, new_state} transition, alongside the matching
// recordResult() (cu_dispatch_unit.h) - slot isn't needed here, only the
// state, since this counts, it doesn't route.
inline void barrierOnEvent(BarrierState& b, WarpStatusCode code) {
    if (code == WarpStatusCode::COMPLETE) {
        ++b.done_count_;
    } else {   // STALLED_AT_BARRIER
        ++b.stalled_count_;
    }
}

// True once every warp in the kernel is simultaneously stalled at a
// barrier - matches simulationProcess()'s barrier_queue.size() ==
// total_warps_ check exactly, global across all CUs.
inline bool barrierReleaseReady(const BarrierState& b) {
    return b.total_warps_ != 0 && b.stalled_count_ == b.total_warps_;
}

// Call after broadcasting release (releaseBarrierSlots(), cu_dispatch_unit.h)
// - resets the arrival counter for the kernel's next wave, if any.
// done_count_ is untouched; it accumulates across the whole kernel.
inline void barrierAcknowledgeRelease(BarrierState& b) {
    b.stalled_count_ = 0;
}

// True once every warp in the kernel has reached COMPLETE, across every
// wave - the whole kernel is finished.
inline bool barrierKernelComplete(const BarrierState& b) {
    return b.total_warps_ != 0 && b.done_count_ == b.total_warps_;
}

// ── Real, multi-CU global barrier arbiter (docs/hls/interfaces.md SS16.37) ──
// The one genuinely new piece needed for NUM_CUS>1: BarrierState is
// inherently kernel-wide (release/completion depend on every CU's warps
// together, not any one CU's own share), so it can't simply be duplicated
// per-CU the way schedulerCore's own WarpSlot[] correctly is - each CU
// deciding release "locally" would release its own warps without waiting
// for the others, silently wrong. barrierCore owns the one real
// BarrierState and is the sole source of the top-level busy/done/fault
// signals - schedulerCore no longer owns any of the three.
//
// Broadcast from barrierCore to each CU's schedulerCore, one entry per
// pending action - deliberately two independent bools, not an enum, since
// a release and a kernel-done can both be true possibilities the receiver
// needs to check every pass (though not simultaneously in practice: a
// release fires mid-kernel while warps are still resident, done fires only
// once every warp has reached COMPLETE).
struct barrier_signal_t {
    bool release;      // un-stall my resident STALLED slots
    bool kernel_done;  // whole kernel (every CU) finished - return to IDLE
};

// TEMPORARY DIAGNOSTIC: widened from ap_uint<8> to ap_uint<32> (see
// barrierCoreN's comment below) so programLoader's seed-loop progress
// (seed_c/seed_slot/seed_i) can be packed into the upper bits and read
// directly via the existing status GPIO - revert to ap_uint<8> once the
// busy-forever investigation concludes (also revert axis_status_latch.v's
// data width and build_all.tcl's scheduler_status_gpio_0 C_GPIO_WIDTH back
// to 8).
using scheduler_status_t = ap_uint<32>;

inline scheduler_status_t packSchedulerStatus(bool busy, bool done, bool fault,
                                              bool ready) {
    scheduler_status_t status = 0;
    status[0] = busy;
    status[1] = done;
    status[2] = fault;
    status[3] = ready;
    return status;
}

// Free-running top-level task, same persistent-hardware model every other
// free-running kernel in this project already uses. Mirrors mem_arbiter's
// proven N:1/1:N array-of-streams shape (docs/hls/interfaces.md SS10.9) -
// NUM_CUS event-in streams (one per CU, forwarding the WarpStatusCode each
// CU's schedulerCore already extracted from its own status_in for its own
// recordResult() call - not re-deriving anything, just relaying), NUM_CUS
// signal-out streams (release/done broadcasts).
//
// Independently observes the SAME start/total_warps every CU's own
// schedulerCore also observes (matching how program_ptr/program_len are
// already broadcast to every CU, not routed, SS10.8) - no explicit
// "launch" message needed from any CU. Each CU's schedulerCore
// independently recomputes the identical launch-fault condition
// (total_warps > NUM_CUS*MAX_WARPS_PER_CU) before self-launching, so a
// faulted launch never causes a CU to dispatch warps that barrierCore
// itself refused to accept - a pure, stateless check every caller derives
// the same way, not a value that needs relaying.
//
// NUM_CUS <= 8: flat DATAFLOW - one stream per CU (N = NUM_CUS).
// NUM_CUS >  8: hierarchical DATAFLOW - one stream per cluster (N = NUM_CLUSTERS).
// In the hierarchical case clusterBarrierRelay (cu_cluster.h) multiplexes
// all per-CU events onto the single cluster stream; barrierCore's counting
// logic is identical regardless of N.
template<int N>
inline void barrierCoreN(
    warp_id_t total_warps,
    bool&     start,
    hls::stream<scheduler_status_t>& status_out,
    hls::stream<WarpStatusCode>   (&events_in)[N],
    hls::stream<barrier_signal_t> (&signal_out)[N],
    // TEMPORARY DIAGNOSTIC (see below): programLoader's phase markers
    // (1/2) AND periodic seed-loop progress updates (bit31 set: bits
    // [3:0]=seed_c, [7:4]=seed_slot, [19:8]=seed_i), written BEFORE each
    // potentially-blocking m_axi read so the last value seen here is
    // exactly where programLoader froze, if it froze.
    hls::stream<ap_uint<32> >& phase_debug_in,
    // TEMPORARY DIAGNOSTIC: one-shot pipeline-stage markers from CU0's
    // schedulerCore (tags 1/2/6/7) and compute_pipeline (tags 3/4/5) - see
    // the bit4-14 comment below. Index-0 channels only; CU1-7 write at
    // most a harmless single "running" tag into their OWN sched_debug[c]/
    // cp_debug[c] channel (never read here, never overflows since it's a
    // single write per run for a total_warps=1 diagnostic kernel).
    hls::stream<ap_uint<4> >&  sched_debug_in,
    hls::stream<ap_uint<4> >&  cp_debug_in
) {
    BarrierState barrier;
    bool busy_state = false;
    bool done_state = false;
    bool fault_state = false;
    bool launch_armed = true;
    scheduler_status_t last_status = packSchedulerStatus(false, false, false, true);
    status_out.write(last_status);

    // TEMPORARY DIAGNOSTIC BITS (bits 4-6, see /memories/session/
    // bit3_swap_experiment.md "busy asserts but done never reached"): sticky
    // (never cleared once set), OR'd into every status_out write, to narrow
    // down where a hung kernel is actually stuck without needing a debugger.
    // Remove once that investigation concludes.
    //   bit4 (0x10): launch_seen      - busy_state ever became true (launch fired)
    //   bit5 (0x20): cu0_event_seen   - events_in[0] ever produced ANY WarpStatusCode
    //   bit6 (0x40): phase1_done      - programLoader finished Phase 1 (program words)
    //   bit7 (0x80): cu0_seeded       - programLoader fully seeded CU0's registers
    //   Full CU0 dispatch/execute/relay hop-by-hop trace (bits 8-14, added
    //   after reg_seed depth 4->1024 fixed Phase 2 but done/event still
    //   never arrived - narrows down exactly which hop is broken instead of
    //   guessing through more rebuild cycles):
    //   bit8  (0x0100): sched0_running     - schedulerCore[0] read loaded_in, called launchSlots
    //   bit9  (0x0200): sched0_dispatched  - schedulerCore[0]'s dispatch_out.write() returned
    //   bit10 (0x0400): cp0_dispatch_read  - compute_pipeline[0] read dispatch_in
    //   bit11 (0x0800): cp0_loop_entered   - compute_pipeline[0] about to call executeOneWarp
    //                                        with resume_pc < program_len (will run >=1 iter)
    //   bit12 (0x1000): cp0_status_written - compute_pipeline[0]'s status_out.write() returned
    //   bit13 (0x2000): sched0_status_read - schedulerCore[0] read status_in, called recordResult
    //   bit14 (0x4000): sched0_event_written - schedulerCore[0]'s barrier_events_out.write() returned
    //   bit15 (0x8000): cp0_seed_read - compute_pipeline[0] actually read >=1 entry
    //                    from reg_seed_in. Added because programLoader's Phase 2
    //                    "complete" writes exactly depth=1024 entries when
    //                    total_warps==1 (local_w<total_warps guard skips all other
    //                    slots), so Phase 2 completing does NOT by itself prove
    //                    compute_pipeline[0] ever ran - the 1024 entries could sit
    //                    unread in the FIFO the whole time.
    //   bit16 (0x10000): cp0_loop_alive - compute_pipeline[0]'s while(true) body
    //                    executed at least once (one-shot heartbeat, written
    //                    before any stream check). If this is 0, the DATAFLOW
    //                    canvas never actually started/scheduled this process
    //                    in hardware at all.
    bool launch_seen    = false;
    bool cu0_event_seen = false;
    bool phase1_done    = false;
    bool cu0_seeded     = false;
    bool sched0_running       = false;
    bool sched0_dispatched    = false;
    bool cp0_dispatch_read    = false;
    bool cp0_loop_entered     = false;
    bool cp0_status_written   = false;
    bool sched0_status_read   = false;
    bool sched0_event_written = false;
    bool cp0_seed_read        = false;
    bool cp0_loop_alive       = false;

    while (true) {
#pragma HLS PIPELINE off
        if (!phase_debug_in.empty()) {
            ap_uint<32> marker = phase_debug_in.read();
            if (!marker[31]) {
                if (marker == 1) {
                    phase1_done = true;
                } else if (marker == 2) {
                    cu0_seeded  = true;
                }
            }
            // marker[31] set = seed-loop progress update - already proven
            // Phase 2 completes with reg_seed depth=1024, no longer tracked.
        }
        if (!sched_debug_in.empty()) {
            switch (sched_debug_in.read()) {
                case 1: sched0_running       = true; break;
                case 2: sched0_dispatched    = true; break;
                case 6: sched0_status_read   = true; break;
                case 7: sched0_event_written = true; break;
                default: break;
            }
        }
        if (!cp_debug_in.empty()) {
            switch (cp_debug_in.read()) {
                case 3: cp0_dispatch_read  = true; break;
                case 4: cp0_loop_entered   = true; break;
                case 5: cp0_status_written = true; break;
                case 8: cp0_seed_read      = true; break;
                case 9: cp0_loop_alive     = true; break;
                default: break;
            }
        }
        if (!start) {
            launch_armed = true;
            if (!busy_state) {
                done_state = false;
                fault_state = false;
            }
        }

        if (!busy_state) {
            if (start && launch_armed) {
                launch_armed = false;
                done_state = false;
                barrierLaunch(barrier, total_warps);
                fault_state = barrierLaunchFault(barrier);
                if (!fault_state) busy_state = true;
                if (busy_state) launch_seen = true;
            }
        } else {
        POLL_CU_EVENTS:
            for (int c = 0; c < N; ++c) {
#pragma HLS UNROLL
                if (!events_in[c].empty()) {
                    WarpStatusCode code = events_in[c].read();
                    if (c == 0) cu0_event_seen = true;
                    barrierOnEvent(barrier, code);
                }
            }

            if (barrierReleaseReady(barrier)) {
                barrierAcknowledgeRelease(barrier);
            SIGNAL_RELEASE:
                for (int c = 0; c < N; ++c) {
#pragma HLS UNROLL
                    barrier_signal_t sig;
                    sig.release     = true;
                    sig.kernel_done = false;
                    signal_out[c].write(sig);
                }
            }

            if (barrierKernelComplete(barrier)) {
                busy_state = false;
                done_state = true;
            SIGNAL_DONE:
                for (int c = 0; c < N; ++c) {
#pragma HLS UNROLL
                    barrier_signal_t sig;
                    sig.release     = false;
                    sig.kernel_done = true;
                    signal_out[c].write(sig);
                }
            }
        }

        bool ready_state = (!busy_state) && launch_armed && (!fault_state);
        scheduler_status_t status =
            packSchedulerStatus(busy_state, done_state, fault_state, ready_state);
        status[4] = launch_seen;
        status[5] = cu0_event_seen;
        status[6] = phase1_done;
        status[7] = cu0_seeded;
        status[8]  = sched0_running;
        status[9]  = sched0_dispatched;
        status[10] = cp0_dispatch_read;
        status[11] = cp0_loop_entered;
        status[12] = cp0_status_written;
        status[13] = sched0_status_read;
        status[14] = sched0_event_written;
        status[15] = cp0_seed_read;
        status[16] = cp0_loop_alive;
        if (status != last_status) {
            status_out.write(status);
            last_status = status;
        }
    }
}

// Convenience wrapper: existing flat call sites compile unchanged.
// In the hierarchical path (NUM_CUS > 8) gpgpu_top.cpp calls
// barrierCoreN<NUM_CLUSTERS>(...) directly with the cluster-level streams.
inline void barrierCore(
    warp_id_t total_warps,
    bool& start,
    hls::stream<scheduler_status_t>& status_out,
    hls::stream<WarpStatusCode>   (&events_in)[NUM_CUS],
    hls::stream<barrier_signal_t> (&signal_out)[NUM_CUS],
    hls::stream<ap_uint<32> >& phase_debug_in,
    hls::stream<ap_uint<4> >&  sched_debug_in,
    hls::stream<ap_uint<4> >&  cp_debug_in
) {
    barrierCoreN<NUM_CUS>(total_warps, start, status_out,
                          events_in, signal_out, phase_debug_in,
                          sched_debug_in, cp_debug_in);
}

}  // namespace riscv_gpgpu_hls

#endif  // RISCV_GPGPU_HLS_BARRIER_ARBITER_H
