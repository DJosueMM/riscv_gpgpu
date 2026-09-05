// gpgpu_top.cpp - definition of the real top-level "compute" IP
// (docs/hls/interfaces.md SS15/SS16.6). See gpgpu_top.h for why this is a
// real .cpp, not header-only `inline` like schedulerCore.

#include "gpgpu_top.h"

#if RISCV_GPGPU_NUM_CUS >= 13
#include "cu_cluster.h"   // cuCluster, superMemArbiter, clusterBarrierRelay
#endif

// T024-style per-board tuning (see memory_pipeline.cpp's identical pattern):
// a macro, not constexpr, so it substitutes textually into the #pragma HLS
// INTERFACE m_axi line below. Falls back to 0 (widening disabled - prior
// behavior) if no board macro is defined, so plain csim/no-board builds are
// unaffected.
#ifndef RISCV_GPGPU_MAXI_MAX_WIDEN_BITWIDTH
#define RISCV_GPGPU_MAXI_MAX_WIDEN_BITWIDTH 0
#endif

namespace riscv_gpgpu_hls {

void gpgpu_scheduler(
    instr_word_t* program_ptr,

    // One AXI master port per compute pipeline.
    // Both pointers may later reference the same physical DDR buffer.
    reg_t* initial_regs_ptr0,
    reg_t* initial_regs_ptr1,

    warp_id_t     total_warps,
    uint32_t      _reserved0,
    uint32_t      program_len,
    uint32_t      _reserved1,
    bool          start,
    uint32_t      _reserved2,
    warp_id_t     warp_id_offset,
    uint32_t      _reserved3,
    hls::stream<scheduler_status_t>& status_out,
    hls::stream<mem_req_t>&  mem_req_out,
    hls::stream<mem_resp_t>& mem_resp_in
) {
    // Unused - see the ADDR_BITS-mod-16 CSR write-decode workaround comment
    // on the declaration in gpgpu_top.h. These pad every REAL scalar below
    // onto a mod-16 offset (silently discard writes below).
    (void)_reserved0;
    (void)_reserved1;
    (void)_reserved2;
    (void)_reserved3;
    // One AXI master owns program loading.
#pragma HLS INTERFACE m_axi \
    port=program_ptr \
    offset=slave \
    bundle=gmem0 \
    max_widen_bitwidth=RISCV_GPGPU_MAXI_MAX_WIDEN_BITWIDTH

    // IMPORTANT:
    // CU0 and CU1 use DIFFERENT AXI master bundles.
    // This avoids DATAFLOW multiple-process ownership of a single m_axi port.
#pragma HLS INTERFACE m_axi \
    port=initial_regs_ptr0 \
    offset=slave \
    bundle=gmem1 \
    max_widen_bitwidth=RISCV_GPGPU_MAXI_MAX_WIDEN_BITWIDTH

#pragma HLS INTERFACE m_axi \
    port=initial_regs_ptr1 \
    offset=slave \
    bundle=gmem2 \
    max_widen_bitwidth=RISCV_GPGPU_MAXI_MAX_WIDEN_BITWIDTH

    // See the gpgpu_top.h declaration comment: each pointer's auto-generated
    // control_r block is {DATA_0,DATA_1,CTRL} = 12 bytes (confirmed via
    // regenerated RTL), so naturally-packed pointers land 12 bytes apart
    // (0x10, 0x1c, 0x28) - NOT mod-16-alignable via interleaved dummy
    // padding scalars (dummy scalars in this bundle also cost a fixed 8
    // bytes each - {DATA_0,CTRL} - and 12/8 share no useful common multiple
    // under mod 16, so no combination of dummies bridges a 12-byte pointer
    // block to the next mod-16 boundary; confirmed empirically after two
    // failed padding attempts). Instead, explicitly PIN each pointer's own
    // register offset via the s_axilite `offset=` suboption, 16 bytes apart,
    // bypassing the tool's automatic sequential packing entirely.
#pragma HLS INTERFACE s_axilite port=program_ptr        bundle=control_r offset=0x10
#pragma HLS INTERFACE s_axilite port=initial_regs_ptr0  bundle=control_r offset=0x20
#pragma HLS INTERFACE s_axilite port=initial_regs_ptr1  bundle=control_r offset=0x30

#pragma HLS INTERFACE s_axilite port=total_warps    bundle=control
#pragma HLS INTERFACE s_axilite port=_reserved0      bundle=control
#pragma HLS INTERFACE s_axilite port=program_len    bundle=control
#pragma HLS INTERFACE s_axilite port=_reserved1      bundle=control
#pragma HLS INTERFACE s_axilite port=start          bundle=control
#pragma HLS INTERFACE s_axilite port=_reserved2      bundle=control
#pragma HLS INTERFACE s_axilite port=warp_id_offset bundle=control
#pragma HLS INTERFACE s_axilite port=_reserved3      bundle=control
#pragma HLS INTERFACE s_axilite port=return         bundle=control

#pragma HLS INTERFACE axis port=status_out
#pragma HLS INTERFACE axis port=mem_req_out
#pragma HLS INTERFACE axis port=mem_resp_in

#pragma HLS DATAFLOW

#if RISCV_GPGPU_NUM_CUS >= 13
    // ------------------------------------------------------------------
    // Hierarchical DATAFLOW path (NUM_CUS >= 13)
    //
    // Flat DATAFLOW with NUM_CUS > 8 hits the Vitis HLS ~40-backwards-
    // channel limit: 3 backwards arrays × NUM_CUS = 3×16=48 > 40.
    // The tool silently omits cu_mem_resp_8..15 causing "Illegal connection"
    // in RTL generation.
    //
    // Solution: split into NUM_CLUSTERS=2 cuCluster instances each
    // managing CLUSTER_SIZE=NUM_CUS/2 CUs.  Each cluster's internal
    // DATAFLOW has 3*CLUSTER_SIZE=24 backwards channels (well < 40).
    // The top-level DATAFLOW has only 4 backwards channels (2 cluster_events
    // + 2 cluster_signal from cuCluster→barrierCore and
    // superMemArbiter→cuCluster).
    // ------------------------------------------------------------------

    CuDispatchUnit cu_a[CLUSTER_SIZE];
    CuDispatchUnit cu_b[CLUSTER_SIZE];

    hls::stream<WarpStatusCode>   cluster_events[NUM_CLUSTERS];
    hls::stream<barrier_signal_t> cluster_signal[NUM_CLUSTERS];
    hls::stream<mem_req_t>        cluster_req[NUM_CLUSTERS];
    hls::stream<mem_resp_t>       cluster_resp[NUM_CLUSTERS];

#pragma HLS STREAM variable=cluster_events  depth=MAX_WARPS_PER_CU dim=1
#pragma HLS STREAM variable=cluster_signal  depth=2                dim=1
#pragma HLS STREAM variable=cluster_req     depth=2                dim=1
#pragma HLS STREAM variable=cluster_resp    depth=2                dim=1

    // Barrier controller sees NUM_CLUSTERS (=2) streams instead of NUM_CUS.
    barrierCoreN<NUM_CLUSTERS>(
        total_warps, start, status_out,
        cluster_events, cluster_signal
    );

    // Program loader writes to both cluster arrays.
    programLoaderHier(
        program_ptr, program_len, start, cu_a, cu_b
    );

    // Cluster A: CU 0 .. CLUSTER_SIZE-1
    // Cluster A owns gmem1, cluster B owns gmem2 — no shared AXI bundles.
    cuCluster(
        cu_a, cu_id_t(0), total_warps, start, program_len, warp_id_offset,
        initial_regs_ptr0,
        cluster_events[0], cluster_signal[0],
        cluster_req[0], cluster_resp[0]
    );

    cuCluster(
        cu_b, cu_id_t(CLUSTER_SIZE), total_warps, start, program_len, warp_id_offset,
        initial_regs_ptr1,
        cluster_events[1], cluster_signal[1],
        cluster_req[1], cluster_resp[1]
    );

    // 2-cluster memory router.
    superMemArbiter(
        cluster_req, cluster_resp,
        mem_req_out, mem_resp_in
    );

#else
    // ------------------------------------------------------------------
    // Flat DATAFLOW path (NUM_CUS <= 8)
    // 3 * NUM_CUS <= 24 backwards channels — within the ~40 tool limit.
    // ------------------------------------------------------------------

    // Explicit, uniquely-named per-CU program arrays (NOT a
    // CuDispatchUnit[NUM_CUS] array-of-objects) - matches the cu_regs_0..7
    // pattern below exactly. Root-caused this session: array-of-objects
    // indexed through programLoader's write loop made every element look
    // like the same storage to HLS's DATAFLOW aliasing analysis, which
    // merged all compute_pipeline instances into one serialized process
    // (WARNING 214-475, "due to reads on 'cu_program_s'") - defeating the
    // whole point of independent per-CU pipelines.
    instr_word_t cu_program_0[MAX_PROGRAM_LEN];
#pragma HLS STREAM variable=cu_program_0 type=pipo depth=3
#if RISCV_GPGPU_NUM_CUS >= 2
    instr_word_t cu_program_1[MAX_PROGRAM_LEN];
#pragma HLS STREAM variable=cu_program_1 type=pipo depth=3
#endif
#if RISCV_GPGPU_NUM_CUS >= 3
    instr_word_t cu_program_2[MAX_PROGRAM_LEN];
#pragma HLS STREAM variable=cu_program_2 type=pipo depth=3
#endif
#if RISCV_GPGPU_NUM_CUS >= 4
    instr_word_t cu_program_3[MAX_PROGRAM_LEN];
#pragma HLS STREAM variable=cu_program_3 type=pipo depth=3
#endif

    hls::stream<warp_dispatch_t> dispatch_out[NUM_CUS];
    hls::stream<warp_status_t>   status_in[NUM_CUS];

    hls::stream<mem_req_t>  cu_mem_req[NUM_CUS];
    hls::stream<mem_resp_t> cu_mem_resp[NUM_CUS];

    hls::stream<WarpStatusCode>   barrier_events[NUM_CUS];
    hls::stream<barrier_signal_t> barrier_signal[NUM_CUS];

    hls::stream<reg_seed_t> reg_seed[NUM_CUS];
    hls::stream<bool>       loaded_sig[NUM_CUS];
    // TEMPORARY DIAGNOSTIC stream (see barrier_arbiter.h's barrierCoreN
    // comment) - programLoader's Phase1-done / CU0-seeded markers plus
    // per-iteration seed-loop progress, polled by barrierCore and exposed
    // as status bits 4-27.
    hls::stream<ap_uint<32> > phase_debug;
    // TEMPORARY DIAGNOSTIC streams (see barrier_arbiter.h's barrierCoreN
    // comment) - one-shot pipeline-stage markers from each CU's
    // schedulerCore/compute_pipeline; only index 0 is polled by barrierCore
    // (CU1-7 write at most one harmless "running" tag into their own unread
    // channel, given this session's total_warps=1 diagnostic kernels).
    hls::stream<ap_uint<4> > sched_debug[NUM_CUS];
    hls::stream<ap_uint<4> > cp_debug[NUM_CUS];

#pragma HLS STREAM variable=dispatch_out   depth=2              dim=1
#pragma HLS STREAM variable=status_in      depth=2              dim=1
#pragma HLS STREAM variable=cu_mem_req     depth=2              dim=1
#pragma HLS STREAM variable=cu_mem_resp    depth=2              dim=1
#pragma HLS STREAM variable=barrier_events depth=MAX_WARPS_PER_CU dim=1
#pragma HLS STREAM variable=barrier_signal depth=2              dim=1
// Was depth=4 - hardware diagnostic showed programLoader freezing exactly
// entering seed_i=4 (see barrier_arbiter.h's scheduler_status_t comment),
// matching a full 4-deep FIFO whose consumer wasn't draining it. Widened
// to hold a full warp-slot's worth of seeds (MAX_THREADS_PER_WARP*
// NUM_REGS_PER_THREAD = 32*32 = 1024) so programLoader can never block on
// this stream.
#pragma HLS STREAM variable=reg_seed       depth=1024           dim=1
// HLS 200-1018 suggested depth=2 for this producer/consumer pair.
#pragma HLS STREAM variable=loaded_sig     depth=2              dim=1
#pragma HLS STREAM variable=phase_debug    depth=4
#pragma HLS STREAM variable=sched_debug    depth=4              dim=1
#pragma HLS STREAM variable=cp_debug       depth=4              dim=1

    // ------------------------------------------------------------------
    // Kernel-wide barrier controller
    // ------------------------------------------------------------------

    barrierCore(
        total_warps,
        start,
        status_out,
        barrier_events,
        barrier_signal,
        phase_debug,
        sched_debug[0],
        cp_debug[0]
    );

    // ------------------------------------------------------------------
    // Program loading
    //
    // Only programLoader accesses program_ptr.
    // It broadcasts the same program to the two local CU program stores.
    // ------------------------------------------------------------------

    programLoader(
        program_ptr,
        initial_regs_ptr0,
        initial_regs_ptr1,
        program_len,
        total_warps,
        warp_id_offset,
        start,
        cu_program_0
#if RISCV_GPGPU_NUM_CUS >= 2
      , cu_program_1
#endif
#if RISCV_GPGPU_NUM_CUS >= 3
      , cu_program_2
#endif
#if RISCV_GPGPU_NUM_CUS >= 4
      , cu_program_3
#endif
      ,
        reg_seed,
        loaded_sig,
        phase_debug
    );

    // ------------------------------------------------------------------
    // CU schedulers
    // ------------------------------------------------------------------

    for (int c = 0; c < NUM_CUS; ++c) {
#pragma HLS UNROLL
        cu_id_t cu_id = cu_id_t(c);
        schedulerCore(
            cu_id,
            program_len,
            total_warps,
            start,
            dispatch_out[c],
            status_in[c],
            barrier_events[c],
            barrier_signal[c],
            warp_id_offset,
            loaded_sig[c],
            sched_debug[c]
        );
    }

    // ------------------------------------------------------------------
    // Compute pipelines
    //
    // Register files are declared as explicitly named variables (cu_regs_0..11)
    // rather than cu[c].regsArray() through a loop. HLS DATAFLOW aliasing
    // analysis runs before UNROLL, so indexing through a loop makes every
    // instance appear to access the same BRAM (WARNING 214-475 "Merging
    // processes due to writes on cu_regs_0"). Separate named variables prevent
    // any cross-instance aliasing.
    // ------------------------------------------------------------------

    reg_t cu_regs_0[MAX_WARPS_PER_CU][MAX_THREADS_PER_WARP][NUM_REGS_PER_THREAD];
#pragma HLS ARRAY_PARTITION variable=cu_regs_0 dim=2 complete
    compute_pipeline(cu_id_t(0), dispatch_out[0], cu_program_0, program_len,
                     cu_regs_0, reg_seed[0], cu_mem_req[0], cu_mem_resp[0], status_in[0], cp_debug[0]);
#if RISCV_GPGPU_NUM_CUS >= 2
    reg_t cu_regs_1[MAX_WARPS_PER_CU][MAX_THREADS_PER_WARP][NUM_REGS_PER_THREAD];
#pragma HLS ARRAY_PARTITION variable=cu_regs_1 dim=2 complete
    compute_pipeline(cu_id_t(1), dispatch_out[1], cu_program_1, program_len,
                     cu_regs_1, reg_seed[1], cu_mem_req[1], cu_mem_resp[1], status_in[1], cp_debug[1]);
#endif
#if RISCV_GPGPU_NUM_CUS >= 3
    reg_t cu_regs_2[MAX_WARPS_PER_CU][MAX_THREADS_PER_WARP][NUM_REGS_PER_THREAD];
#pragma HLS ARRAY_PARTITION variable=cu_regs_2 dim=2 complete
    compute_pipeline(cu_id_t(2), dispatch_out[2], cu_program_2, program_len,
                     cu_regs_2, reg_seed[2], cu_mem_req[2], cu_mem_resp[2], status_in[2], cp_debug[2]);
#endif
#if RISCV_GPGPU_NUM_CUS >= 4
    reg_t cu_regs_3[MAX_WARPS_PER_CU][MAX_THREADS_PER_WARP][NUM_REGS_PER_THREAD];
#pragma HLS ARRAY_PARTITION variable=cu_regs_3 dim=2 complete
    compute_pipeline(cu_id_t(3), dispatch_out[3], cu_program_3, program_len,
                     cu_regs_3, reg_seed[3], cu_mem_req[3], cu_mem_resp[3], status_in[3], cp_debug[3]);
#endif

    // ------------------------------------------------------------------
    // N:1 memory arbitration
    // ------------------------------------------------------------------

    mem_arbiter(
        cu_mem_req,
        cu_mem_resp,
        mem_req_out,
        mem_resp_in
    );

#endif  // RISCV_GPGPU_NUM_CUS >= 13
}

}  // namespace riscv_gpgpu_hls
