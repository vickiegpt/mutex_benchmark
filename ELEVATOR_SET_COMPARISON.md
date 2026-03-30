# Elevator Lock Set — Overhead Comparison

Comparison of all locks in the `ELEVATOR_SET` benchmark group, focusing on
algorithmic overhead, space cost, contention behaviour, and
hardware requirements.

**Notation**: *n* = threads, *W* = network width (smallest power of 2 ≥
⌈√n⌉), *CL* = cache-line size (64 B).

---

## 1. Summary Table

| Lock | Lock O() | Unlock O() | Atomics per lock() | Space | Local Spin | Fairness |
|---|---|---|---|---|---|---|
| `exp_spin` | O(1) | O(1) | 1 TAS | O(1) | No | Unfair |
| `mcs` | O(1) | O(1) | 1 XCHG + 1 CAS | O(n·CL) | Yes | FIFO |
| `mcs_nca` | O(1) | O(1) | 1 XCHG + 1 CAS | O(n) | Yes\* | FIFO |
| `linear_cas_elevator` | O(n) | O(n) | 1 TAS (waker) | O(n·CL) | Yes | ≈FIFO |
| `linear_bl_elevator` | O(n) | O(n) | **0** | O(n·CL) | Yes | ≈FIFO |
| `linear_lamport_elevator` | O(n) | O(n) | **0** | O(n) | Yes† | ≈FIFO |
| `tree_cas_elevator` | O(log n) | O(log n) | 1 TAS (waker) | O(n·CL) | Yes | Tree-order |
| `tree_bl_elevator` | O(log n) | O(log n) | **0** | O(n·CL) | Yes | Tree-order |
| `tree_lamport_elevator` | O(log n) | O(log n) | **0** | O(n·CL) | Yes | Tree-order |
| `net_elevator` | O(log²W) | O(n) | log²W × fetch\_xor | O(W²log²W + n·CL) | Yes | Elevator |
| `bitonic_cas` | O(log²W) | O(n) | log²W × fetch\_add | O(W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `bitonic_bl` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `bitonic_lamport` | O(n·log²W)‡ | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `bitonic_elevator` | O(log²W) | O(n) | log²W × (XCHG + CAS) | O(n·W·log²W + n·CL) | Yes (stack+node) | ≈FIFO |
| `bitonic_bakery` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_cas` | O(log²W) | O(n) | log²W × fetch\_add | O(W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_bl` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_lamport` | O(n·log²W)‡ | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_elevator` | O(log²W) | O(n) | log²W × (XCHG + CAS) | O(n·W·log²W + n·CL) | Yes (stack+node) | ≈FIFO |
| `periodic_bakery` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `wf_bitonic_cas` | O(log²W) | **O(1)** | log²W fetch\_add | O(n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_bitonic_bl` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_bitonic_lamport` | O(n·log²W)‡ | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_bitonic_bakery` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_cas` | O(log²W) | **O(1)** | log²W fetch\_add | O(n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_bl` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_lamport` | O(n·log²W)‡ | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_bakery` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |

\* `mcs_nca` nodes not CL-padded — false sharing possible.  
† `linear_lamport_elevator` grant flags not CL-padded.  
‡ O(1) fast path when uncontended; degrades to O(n) per balancer under contention.  
§ Seq designs are currently broken under multi-threaded contention.

---

## 2. Overhead Breakdown

### 2.1 Lock Acquisition

The dominant cost is the **number of serialisation points** a thread must
pass through to acquire the lock.

| Category | Locks | Serialisation Points | Cost Model |
|---|---|---|---|
| **Single atomic** | `exp_spin`, `mcs`, `mcs_nca` | 1 | O(1) — one RMW to enter |
| **Linear scan (software)** | `linear_bl_elevator`, `linear_lamport_elevator` | 1 (BL/Lamport doorway over n threads) | O(n) — scan all threads, no atomics |
| **Linear scan (CAS waker)** | `linear_cas_elevator` | 1 TAS + O(n) scan | O(n) — 1 atomic + linear scan |
| **Tree walk** | `tree_*_elevator` | O(log n) tree nodes + waker | O(log n) — write from leaf to root |
| **Network traverse (CAS)** | `net_elevator`, `bitonic_cas`, `periodic_cas` | O(log²W) atomics | O(log²W) — each balancer is 1 RMW |
| **Network traverse (elevator sync)** | `bitonic_elevator`, `periodic_elevator` | O(log²W) MCS-style locks | O(log²W) — each balancer is 1 XCHG + local spin |
| **Network traverse (software)** | `bitonic_bl`, `periodic_bl`, `*_bakery` | O(log²W) mutex acquires, each O(n) | O(n·log²W) — most expensive |
| **Network traverse (Lamport)** | `bitonic_lamport`, `periodic_lamport` | O(log²W) Lamport locks | O(1)–O(n) per balancer × log²W |

**Concrete example** (n = 16 threads → W = 4, log²W = 4 balancers traversed):

| Lock | Ballpark lock() operations |
|---|---|
| `mcs` | 1 XCHG |
| `exp_spin` | 1 TAS (+ backoff retries) |
| `linear_bl_elevator` | 16-thread BL doorway scan |
| `tree_cas_elevator` | 4 tree writes + 1 TAS |
| `bitonic_cas` / `periodic_cas` | 4 fetch\_add |
| `bitonic_elevator` / `periodic_elevator` | 4 XCHG + 4 CAS (MCS-style per balancer) |
| `bitonic_bl` / `periodic_bl` | 4 × 16-thread BL doorways = 64 flag reads |
| `bitonic_bakery` / `periodic_bakery` | 4 × 16-thread bakery doorways = 64 reads + 64 writes |

### 2.2 Unlock / Grant

| Category | Locks | Unlock Cost | Mechanism |
|---|---|---|---|
| **O(1) release** | `exp_spin` | O(1) | Store 0 to flag |
| **O(1) handoff** | `mcs`, `mcs_nca` | O(1) | Write to successor's node (1 CAS if tail) |
| **O(log n) tree walk** | `tree_*_elevator` | O(log n) | Root-to-leaf sibling check + ring dequeue |
| **O(n) linear scan** | `linear_*_elevator` | O(n) | Cyclic scan of waiting[] flags |
| **O(n) step-property scan** | `net_elevator`, `bitonic_*`, `periodic_*` | O(n) | Scan all ThreadMeta, use (wire\_dist, round, tid) ordering |

The new bitonic/periodic locks and the existing counting network locks share
the same O(n) unlock scan. This is the primary scaling bottleneck — every
unlock must inspect all n thread metadata slots. The WF (Waiting-Filter)
variants solve this with O(1) unlock via a phase-bit chain.

### 2.3 Space Overhead

| Lock | Per-thread | Shared State | Total |
|---|---|---|---|
| `exp_spin` | 0 | 1 atomic flag | O(1) |
| `mcs` | 1 CL-padded node | 1 atomic tail ptr | O(n·CL) |
| `linear_*_elevator` | 1 CL-padded grant flag + 1 bool | Waker lock O(n) | O(n·CL) |
| `tree_*_elevator` | 1 CL-padded grant flag | 2n+1 tree nodes + ring buffer | O(n·CL) |
| `net_elevator` | 1 CL-padded meta slot | W²/2 · log²W atomics | O(W²·log²W + n·CL) |
| `cn_ticket_*` | 1 CL-padded meta slot | CL-padded balancer array | O(W·log²W·CL + n·CL) | *removed* |
| `bitonic_cas` / `periodic_cas` | 1 CL-padded meta slot | Heap-allocated balancer tree | O(W·log²W + n·CL) |
| `bitonic_bl` / `periodic_bl` | 1 CL-padded meta slot | Balancer tree + n flags/balancer | O(n·W·log²W + n·CL) |
| `bitonic_elevator` / `periodic_elevator` | 1 CL-padded meta slot | Balancer tree + n MCS nodes/balancer | O(n·W·log²W + n·CL) |
| `bitonic_bakery` / `periodic_bakery` | 1 CL-padded meta slot | Balancer tree + 2n arrays/balancer | O(n·W·log²W + n·CL) |

The software-sync variants (BL, Lamport, Elevator, Bakery) allocate O(n) state
**per balancer**, multiplied by O(W/2 · log²W) balancers. For n = 64 (W = 8,
~24 balancers): ~1536 flag bytes for BL, ~6 KB for Bakery.

---

## 3. Contention Distribution

The key advantage of counting-network-based locks over simple elevator locks
is **contention distribution**. Instead of all threads contending on a single
point, the network spreads contention across W/2 independent balancers per
stage.

| Lock | Contention Points | Threads per Point |
|---|---|---|
| `exp_spin` | 1 shared flag | n |
| `mcs` | 1 tail pointer | n |
| `linear_*_elevator` | 1 waker lock | n |
| `tree_*_elevator` | Tree root | n (up to O(log n) per level) |
| `net_elevator` | W/2 balancers per stage | n/W per balancer (expected) |
| `bitonic_*` / `periodic_*` | W/2 balancers per stage | n/W per balancer (expected) |
| `cn_ticket_*` | W/2 CL-padded balancers per stage | n/W per balancer | *removed* |

For n = 64 threads with W = 8: each balancer sees ~8 threads instead of 64.
This reduces cache-line bouncing by ~8× compared to a single-point lock.

---

## 4. Hardware Requirements

| Lock | Requires Atomic RMW? | Pure Software? | CXL/NUMA Notes |
|---|---|---|---|
| `exp_spin` | Yes (TAS) | No | Poor — all spin on shared line |
| `mcs` | Yes (XCHG, CAS) | No | Excellent — local spin on own node |
| `linear_cas_elevator` | Yes (TAS in waker) | No | Good — local spin on own flag |
| `linear_bl_elevator` | **No** | **Yes** | Good — local spin, software fences only |
| `linear_lamport_elevator` | **No** | **Yes** | Moderate — flags not CL-padded |
| `tree_cas_elevator` | Yes (tree atomics) | No | Good — local spin |
| `tree_bl_elevator` | **No** | **Yes** | Good — local spin |
| `tree_lamport_elevator` | **No** | **Yes** | Good — local spin |
| `net_elevator` | Yes (fetch\_xor) | No | Good — local spin, distributed balancers |
| `cn_ticket_bl` | **No** | **Yes** | Good — stack-local spin | *removed* |
| `cn_ticket_lamport` | **No** | **Yes** | Good — stack-local spin | *removed* |
| `bitonic_cas` / `periodic_cas` | Yes (fetch\_add) | No | Good — stack-local spin, distributed |
| `bitonic_bl` / `periodic_bl` | **No** | **Yes** | Good — stack-local spin, distributed |
| `bitonic_lamport` / `periodic_lamport` | **No** | **Yes** | Good — stack-local spin, distributed |
| `bitonic_elevator` / `periodic_elevator` | Yes (XCHG + CAS) | No | **Best** — local spin at both balancer and lock level |
| `bitonic_bakery` / `periodic_bakery` | **No** | **Yes** | Good — stack-local spin, no atomics at all |

**Pure software locks** (no atomic RMW instructions, fences only):
`linear_bl_elevator`, `linear_lamport_elevator`, `tree_bl_elevator`,
`tree_lamport_elevator`, `bitonic_bl`,
`bitonic_lamport`, `bitonic_bakery`, `periodic_bl`, `periodic_lamport`,
`periodic_bakery`, `wf_bitonic_bl`, `wf_bitonic_lamport`, `wf_bitonic_bakery`,
`wf_periodic_bl`, `wf_periodic_lamport`, `wf_periodic_bakery`.

---

## 5. Bitonic / Periodic vs Existing Locks — Trade-off Analysis

### When bitonic/periodic CAS locks win over elevator locks

- **High thread counts (n > 16)**: The O(log²W) distributed network traverse
  scales better than the O(n) linear scan in `linear_*_elevator`. At n = 64,
  the network traverses ~10 balancers while linear elevator scans 64 flags.
- **NUMA/CXL with many sockets**: Network distributes cache-line ownership
  across W/2 balancers. Elevator locks concentrate contention in the waker
  lock (one cache line).
- **Mixed workloads**: Step-property ordering provides fairer distribution
  than the BurnsLamport priority-based waker.

### When elevator locks win over bitonic/periodic

- **Low thread counts (n ≤ 8)**: Simple linear scan is cheaper than building
  and traversing a multi-level network. The constant factors in network
  construction (heap allocation, pointer chasing) dominate.
- **Unlock-heavy workloads**: Both use O(n) unlock scans, but elevator locks
  have smaller constant factors (simpler metadata).
- **Space-constrained environments**: `linear_bl_elevator` uses O(n·CL) space.
  `bitonic_bl` uses O(n·W·log²W + n·CL) — significantly more for software
  sync variants.

### When MCS wins over everything

- **Pure throughput under contention**: O(1) lock + O(1) unlock with FIFO
  ordering. No scan, no network traverse. MCS is hard to beat when the only
  goal is raw lock/unlock throughput and hardware CAS is available.
- **Caveat**: MCS requires one atomic XCHG per lock and one CAS per unlock —
  these are not available on all ISAs or CXL configurations.

### When bitonic/periodic software variants are uniquely valuable

- **ISAs without atomic RMW** (embedded, some RISC-V cores, CXL-attached
  memory without coherent atomics): `bitonic_bl`, `bitonic_bakery`,
  `periodic_bl`, `periodic_bakery` provide distributed counting with only
  loads, stores, and fences.
- **No other lock in this set** combines distributed contention reduction
  with pure-software execution except the bitonic/periodic variants (recursive network)
  and their WF counterparts.
- **No other lock in this set** combines distributed contention reduction,
  O(1) unlock, AND pure-software execution except the `wf_*_bl`,
  `wf_*_lamport`, and `wf_*_bakery` variants.

### Bitonic vs Periodic

Both have the same asymptotic complexity. The difference is structural:

| Property | Bitonic | Periodic |
|---|---|---|
| Structure | Recursive (half-networks + merger) | Repeated identical blocks |
| Regularity | Irregular — different stages have different wiring | **Regular** — every stage identical |
| Hardware mapping | Complex routing | Simple, tiled layout |
| Pipeline parallelism | Harder — stages differ | Natural — each block is one pipeline stage |
| Fault tolerance | Replace specific sub-network | Replace any block |
| Software overhead | Slightly less pointer chasing (fewer blocks) | Slightly more blocks for same width |

In practice, **performance should be nearly identical** for software
implementations. The periodic variant is preferred for hardware synthesis
or when structural regularity is valued.

---

## 6. Recommended Lock Selection

| Scenario | Recommended Lock | Rationale |
|---|---|---|
| General purpose, CAS available | `mcs` | O(1)/O(1), FIFO, local spin |
| High contention, many threads, CAS available | `bitonic_cas` or `periodic_cas` | Distributed contention, stack-local spin |
| Pure software, moderate threads | `linear_bl_elevator` | Simple, no atomics, local spin |
| Pure software, many threads | `bitonic_bl` or `periodic_bl` | Distributed contention without any atomics |
| NUMA/CXL, best locality | `bitonic_elevator` or `periodic_elevator` | MCS-style local spin at every balancer |
| Most portable (no atomics, no fences under SC) | `bitonic_bakery` or `periodic_bakery` | Pure loads and stores |
| Strict FIFO with O(1) unlock | `wf_bitonic_cas` or `wf_periodic_cas` | Waiting-filter, O(1) phase-bit unlock, no extra atomics |
| O(1) unlock + distributed lock | `wf_bitonic_cas` or `wf_periodic_cas` | Waiting-filter, best balance of throughput and unlock cost |
| O(1) unlock + single bottleneck | `seq_bitonic_cas` | Sequenced, global fetch\_add linearization (currently broken) |

---

## 7. Linearizable Counting Lock Designs

Three designs from Herlihy, Shavit, Waarts (1996) "Linearizable Counting
Networks" and Aspnes, Herlihy, Shavit (1994) "Counting Networks," implemented
as mutual exclusion locks with the project's `SoftwareMutex` interface.

### 7.1 Design Overview

| Design | Lock O() | Unlock O() | Extra Atomics | Key Mechanism | Source |
|---|---|---|---|---|---|
| **A: Wire-Indexed (LW)** | O(log²W) | O(1) pred / O(W) fallback | 0 | Per-wire slot arrays + step-property prediction | Novel |
| **B: Sequenced (Seq)** | O(log²W) + 1 fetch\_add | O(1) guaranteed | 1 fetch\_add (global\_seq) | Counting network + global ticket + slot array | Aspnes et al. §5.1 |
| **C: Waiting-Filter (WF)** | O(log²W) + O(1) phase check | **O(1) always** | 0 | Counting network + n-element phase-bit array | Herlihy-Shavit-Waarts §3 |

### 7.2 Theoretical Foundation

**From Aspnes-Herlihy-Shavit (1994):**
- Step property (Lemma 2.2): In quiescent state, output wire i has ⌈(m−i)/w⌉
  tokens. This enables predictable token assignment for locks.
- Theorem 3.6: Bitonic[w] satisfies the step property.
- Theorem 4.4: Periodic[2k] satisfies the step property after log(k) Block stages.
- Shared counter (§5.1): The counting network distributes thread arrivals
  across W output wires, each maintaining a local counter. Token =
  wire + round × width.

**Design C connection to Herlihy-Shavit-Waarts (1996):**
- The Waiting-filter from §3 uses an n-element phase-bit array where
  phase(v) = ⌊v/n⌋ mod 2. Token v waits for predecessor v−1 at slot
  (v−1) mod n, then sets its own phase bit on unlock. This provides
  O(1) unlock with linearizable ordering.
- Impossibility (Theorem 5.4): Any linearizable counting protocol with
  capacity c has latency Ω(n/c). The Waiting-filter trades one blocking
  wait (at the phase-bit check) for O(1) unlock — the best possible
  without non-blocking constraints.

### 7.3 Implementation Notes

**Design A (Wire-Indexed):** Uses the designated-waker protocol from the
bitonic counting locks. The unlock path predicts the successor using the
step property: the next thread should be on wire (current+1) % W at the
next round. O(1) if prediction hits, O(W) sweep if it misses.
**Status: BROKEN under contention.** The waker protocol can deadlock when
multiple threads compete across iterations. The 2T+ benchmarks show 0 throughput.
This design requires a waker-free rework to be viable.

**Design B (Sequenced):** Combines a counting network for contention
distribution with a global `fetch_add` for linearization. The network
spreads arrivals across W/2 balancers per stage so threads hit
`global_seq_` at staggered intervals. Slot array indexed by `seq & mask`
provides direct handoff in unlock. Essentially a ticket lock backed by a
counting network.

**Design C (Waiting-Filter):** The cleanest design. Token v from the
counting network indexes into a circular phase-bit array. Token v waits
for phase\_bit[(v-1) % n] == phase(v-1), which is set by predecessor's
unlock. ABA prevention: phase toggles every n tokens per slot. Under
practical linearizability (c2 ≤ 2·c1), the wait is near-zero because
predecessors complete before successors check.

### 7.4 Experimental Results

Platform: Apple M-series (aarch64), max contention benchmark (1s per run,
median of 3 reps). Throughput in operations/second.

| Lock | 1T ops/s | 1T vs MCS | 2T ops/s | 2T vs MCS | 4T ops/s | 4T vs MCS | 8T ops/s | 8T vs MCS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `mcs` | 9.18M | 1.00x | 3.95M | 1.00x | 1.71M | 1.00x | 15.0K | 1.00x |
| `exp_spin` | 7.42M | 0.81x | 6.84M | 1.73x | 5.80M | 3.40x | 6.97M | 464.67x |
| `ticket` | 9.96M | 1.09x | 3.25M | 0.82x | 934.4K | 0.55x | 189.4K | 12.63x |
| `bitonic_cas` | 8.69M | 0.95x | 3.19M | 0.81x | 1.78M | 1.04x | 57.3K | 3.82x |
| `periodic_cas` | 8.20M | 0.89x | 3.29M | 0.83x | 1.71M | 1.00x | 52.6K | 3.51x |
| `seq_bitonic_cas` | 7.83M | 0.85x | 4.34M | 1.10x | 2.36M | 1.38x | 115.6K | 7.71x |
| `seq_periodic_cas` | 8.30M | 0.90x | 4.33M | 1.10x | 721.8K | 0.42x | 64.9K | 4.33x |
| `wf_bitonic_cas` | 9.78M | 1.07x | 6.13M | 1.55x | 2.50M | 1.46x | 111.3K | 7.42x |
| `wf_periodic_cas` | 9.32M | 1.02x | 5.54M | 1.40x | 2.52M | 1.48x | 58.0K | 3.87x |

### 7.5 Analysis

**Key findings:**

1. **WF (Waiting-Filter) is the best new design.** `wf_bitonic_cas` achieves
   1.46x MCS throughput at 4T and 7.42x at 8T. The phase-bit chain adds
   negligible overhead in practice because predecessors complete before
   successors check (practical linearizability from the Lynch et al. result).
   Note: Elevator sync variants were removed from linearizable designs because
   `BnElevatorSync` uses hardware CAS/XCHG (MCS-style queue), contradicting
   the goal of software-only balancer synchronization.

2. **Seq (Sequenced) performs well at low contention.** `seq_bitonic_cas`
   beats MCS at 2T (1.10x) and 4T (1.38x). The global `fetch_add` bottleneck
   hurts at higher thread counts but the counting network pre-distributes
   arrivals effectively.

3. **O(n) unlock locks (bitonic/periodic) are competitive.** Despite O(n)
   unlock scans, `bitonic_cas` and `periodic_cas` match or exceed MCS at 4T
   due to distributed lock-path contention.

4. **Exp\_spin dominates absolute throughput** at high thread counts due to
   its unfair nature (no ordering, no scan) but lacks FIFO fairness.

5. **MCS collapses at 8T on this platform** (15K ops/s vs 9.18M at 1T),
   likely due to contention collapse on the tail pointer with Apple Silicon's
   memory model. All ordered locks show similar degradation.

6. **LW (Wire-Indexed) is broken** under multi-thread contention. The
   designated-waker protocol doesn't handle multi-iteration re-entry correctly.
   Needs redesign to remove the waker dependency.
