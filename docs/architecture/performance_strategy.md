# Performance and Interface Decision Strategy

**Status**: Proposed planning baseline
**Last updated**: 2026-08-20
**Related tasks**: T096a-T096g, T100, T101, T105, T107, T112-T119

This document defines how performance-sensitive architecture decisions are
made. It deliberately asks whether an interface or microarchitecture is the
right one before optimizing its parameters. The answer must come from a stated
workload, an analytical ceiling, measurements, and a recorded decision.

## 1. First Principle: Separate the Planes

No single AXI variant is best for every purpose.

| Plane | Default mechanism | Why | Trigger to reconsider |
|---|---|---|---|
| Low-rate control and status | AXI4-Lite CSR block | Small, deterministic, easy to drive and verify | Launch/control overhead exceeds 1% of end-to-end time for a target workload |
| Command submission at high launch rate | Memory-resident descriptor queue plus AXI4-Lite doorbell | Amortizes register transactions and supports asynchronous launches | Required only if microkernel measurements show AXI4-Lite launch serialization matters |
| Program, context, and bulk data | Full AXI4 memory-mapped access through managed buffers/DMA | Supports bursts, multiple outstanding transactions, and platform memory | Reconsider width, ports, and topology when sustainable bandwidth or latency misses the workload target |
| Internal request/response traffic | AXI4-Stream or an equivalent ready/valid protocol | Natural backpressure and decoupled modules | Add tags/credits or split streams when one-outstanding ordering limits concurrency |
| Snapshot counters | AXI4-Lite | Low volume and infrequent access | Use a consistent snapshot/latch mechanism if counters cross clock domains |
| High-volume trace | AXI4/AXI4-Stream into a ring buffer or DMA path | Trace traffic can overwhelm the control plane | Sampling may be used only with a documented accuracy trade-off |

AXI4-Lite is therefore the preferred default for configuration, start,
completion, faults, and a bounded counter snapshot. It is not a data transport
and must not carry kernel payloads, result arrays, program images, or event
traces.

## 2. Current Baseline and Working Hypotheses

These are observations to validate, not permanent design decisions.

| Observation | Performance question | Working hypothesis |
|---|---|---|
| The scheduler exposes scalar controls through HLS-generated AXI4-Lite banks | Is CSR launch latency relevant? | It is negligible for long kernels but may dominate fine-grained launches |
| Four AXI masters converge through one SmartConnect into Kria HPC0 | Does arbitration or one physical DDR port cap scaling? | Program/context traffic is launch-time only; runtime load/store traffic will dominate steady state |
| The runtime memory path uses one N:1 CU arbiter and one memory pipeline | Can bandwidth scale from 1 to 8 CUs? | The shared serialized service point will saturate before DDR bandwidth |
| Each CU blocks after one memory request until its response returns | Can HLS `num_*_outstanding=16` create real concurrency? | The source protocol prevents most requested AXI concurrency from being exercised |
| Memory operations are emitted per lane and serviced sequentially | Does a 128-bit AXI beat translate into useful lane throughput? | Width alone cannot recover bandwidth without coalescing or multiple in-flight requests |
| L2 misses fetch one 128-byte line; stores are write-through | Is the traffic amplification appropriate for target workloads? | Streaming reads can use bursts, while sparse reads and write-heavy kernels may waste bandwidth |
| Kria is configured for 128-bit widening at 200 MHz | What is the achievable platform ceiling? | Raw interface capacity is 3.2 GB/s before protocol, arbitration, DDR, and access-pattern losses |
| U55C provides multiple HBM pseudo-channels | Will one shared memory port waste HBM parallelism? | The Alveo shell will need explicit bank placement and multiple independent ports |

The main current bottleneck hypothesis is the serialized memory request path,
not AXI4-Lite. T096c-T096e and T105 must be able to falsify that hypothesis.

## 3. Quantitative Model

Every target profile shall publish these ceilings before implementation
tuning:

- Raw port bandwidth: $B_{raw} = (W / 8) \times f$, where $W$ is AXI data
  width in bits and $f$ is the interface clock.
- Sustainable bandwidth: measured read, write, and mixed traffic after AXI,
  interconnect, memory-controller, and access-pattern effects.
- Peak compute: operations per cycle multiplied by active lanes, compute units,
  and achieved clock.
- Operational intensity: $I = operations / bytes$ at the external-memory
  boundary.
- Roofline bound: $P \leq min(P_{peak}, I \times B_{sustained})$.
- Control overhead: setup, launch, completion, and synchronization time as a
  percentage of end-to-end latency.
- Efficiency: achieved throughput divided by the relevant measured ceiling,
  never by an undocumented marketing peak.

For Kria, 128 bits at 200 MHz gives a raw single-port value of 3.2 GB/s. This
is only an upper bound. The project must measure the sustainable value with the
same allocation, coherency, and AXI path used by kernels.

## 4. Required Architecture Questions

| Gate | Question | Alternatives to measure | Required evidence |
|---|---|---|---|
| T096a Workload envelope | Are we optimizing throughput, latency, launch rate, energy, or a balanced score? | Streaming, irregular, synchronization-heavy, compute-bound, and microkernel classes | Dataset sizes, operational intensity, target metric, and acceptable regression per class |
| T096b Control plane | Is AXI4-Lite sufficient for launch and status? | Direct CSR launch versus descriptor queue plus doorbell; polling versus interrupt | Launches/s, setup latency, CPU cost, kernel-duration crossover, and control percentage of end-to-end time |
| T096c Data plane | Which width, burst, alignment, outstanding depth, and port count are useful? | 64/128-bit Kria configurations; platform-native U55C widths; burst and outstanding sweeps | Read/write/mixed GB/s, latency distribution, AXI utilization, stalls, and resource/timing cost |
| T096d Memory concurrency | Where should requests queue, arbitrate, coalesce, and reorder? | Current one-outstanding N:1 path, tagged multi-outstanding path, per-cluster ports, and banked memory pipelines | 1/2/4/8-CU scaling, fairness, queue occupancy, latency hiding, coalescing ratio, and starvation checks |
| T096e Memory hierarchy | Which cache and write policy fits each workload class? | Cache line sizes, L1/L2 capacities, write-through/write-back, write-allocate policy, scratchpad allocation | Hit rates, traffic amplification, useful bytes per burst, stalls, BRAM/URAM cost, and correctness/coherence tests |
| T096f Platform topology | Which physical ports and memory banks should each logical region use? | Kria HP/HPC and port count; U55C HBM pseudo-channel and CU placement choices | Sustainable bandwidth, coherency behavior, SmartConnect/HBM contention, SLR crossings, timing, and power |
| T096g Compute balance and observability | Are compute, scheduler, memory, and telemetry balanced and measurable? | CU/lane/warp/clock profiles and counter/trace placements | IPC, lane utilization, issue stalls by cause, memory stalls, barrier/divergence cost, counter overhead, and roofline position |

## 5. Decision Record Template

Each performance-sensitive task and ADR shall contain:

1. **Question**: the uncertainty that can change the architecture.
2. **Workload scope**: the workload classes and input sizes affected.
3. **Alternatives**: including the current baseline and a simpler option.
4. **Hypothesis**: a falsifiable prediction, not a preference.
5. **Metrics**: correctness first, then latency, throughput, utilization,
   resources, timing, power, and engineering cost as applicable.
6. **Threshold**: the measured condition that selects or rejects an option.
7. **Experiment**: fixed configuration, tool/device versions, counters, raw
   data, and repetitions.
8. **Decision**: selected option, rejected alternatives, limits, and revisit
   trigger.

An optimization task without a baseline and a rejection threshold is not
ready for implementation.

## 6. Measurement Sequence

1. Measure host-to-memory copy and memory-controller ceilings without the
   accelerator core.
2. Measure AXI read, write, and mixed traffic through the exact platform shell.
3. Measure one CU with cache-hit, streaming-miss, random-miss, and write-heavy
   microbenchmarks.
4. Sweep CUs and memory topology while collecting queue, arbitration, cache,
   and AXI counters.
5. Compare achieved performance to the roofline and identify the first
   saturated resource.
6. Change one architecture variable at a time and record resource/timing cost.
7. Validate the selected option with representative applications, not only
   synthetic traffic.

## 7. Stop Conditions

- Do not widen AXI if requests remain serialized and measured bus utilization
  is low.
- Do not add CUs when throughput no longer scales or the memory-stall fraction
  grows without a corresponding application-level gain.
- Do not add caches solely because BRAM/URAM is available; require reduced
  external traffic or latency on a target workload class.
- Do not use coherent ports without proving the software allocation and cache
  ownership protocol.
- Do not copy the Kria memory topology to U55C; map independent HBM channels
  deliberately.
- Do not accept a higher clock if achieved application throughput or energy
  efficiency regresses after routing.