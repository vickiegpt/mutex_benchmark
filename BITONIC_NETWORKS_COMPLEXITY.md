# Bitonic Counting Networks — Complexity & Reference

Implementation of Herlihy's bitonic counting networks (Ch. 12, "The Art of
Multiprocessor Programming") with support for differing hardware ISA memory
models and synchronisation primitives.

---

## Algorithmic Complexity Table

All networks have width **w** (must be a power of 2).  
**n** = number of threads, **w** = network width.

| Component | Depth (balancers traversed) | Total Balancers | Width | Traverse Cost per Token |
|---|---|---|---|---|
| **Balancer** | 1 | 1 | 2 | O(1) |
| **Merger[2k]** | O(log 2k) = O(log w) | O(k · log w) | 2k | O(log w) |
| **Bitonic[w]** | O(log²w) | O(w/2 · log²w) | w | O(log²w) |
| **Block[w]** | O(log w) | O(w/2 · log w) | w | O(log w) |
| **Periodic[w]** | O(log²w) | O(w/2 · log²w) | w | O(log²w) |

### Per-Balancer Synchronisation Overhead

| Sync Policy | Per-traverse Cost | RMW Atomics Required? | Space per Balancer |
|---|---|---|---|
| **CAS** (fetch_add) | O(1) amortised | Yes (LOCK prefix / LDXR-STXR) | O(1) — single atomic counter |
| **Burns-Lamport** | O(n) doorway + O(n) wait | No — loads, stores, fences only | O(n) — per-thread flags |
| **Lamport Fast** | O(1) fast path, O(n) slow | No — loads, stores, fences only | O(n) — per-thread arrays |
| **Elevator (Buhr)** | O(1) enqueue + local spin | Single CAS at enqueue | O(n) — per-thread queue nodes |
| **Bakery** | O(n) doorway + O(n) wait | No — pure loads & stores (SC) | O(n) — choosing[] + number[] |

### End-to-End Lock Complexity

| Lock Variant | Network Traverse | Lock (total) | Unlock | Space |
|---|---|---|---|---|
| **bitonic_{sync}** | O(log²w) × T_sync | O(log²w × T_sync + n) | O(n) scan | O(w · log²w + n) |
| **periodic_{sync}** | O(log²w) × T_sync | O(log²w × T_sync + n) | O(n) scan | O(w · log²w + n) |

Where **T_sync** is the per-balancer synchronisation cost from the table above.

---

## Network Implementations

### 1. Balancer (Herlihy Fig 12.10, 12.14)

**What**: A toggle switch with two input and two output wires.  Tokens
alternate between top (wire 0) and bottom (wire 1) outputs.

**Correctness**: In any quiescent state, |y₀ - y₁| ≤ 1.

**Application**: The atomic building block.  Any counting, sorting, or
load-balancing network is composed of balancers.

**Implementation**: `Balancer<Sync>` in `bitonic_networks.hpp`.  The
`traverse(tid)` method is the critical section — it must be serialised.

---

### 2. Merger[2k] (Herlihy Fig 12.12, 12.15)

**What**: Merges two width-k step-property sequences into one width-2k
step-property sequence.

**Construction** (recursive):
- Base (k=1): single balancer.
- Recursive: two Merger[k] sub-networks merge interleaved subsequences,
  then k balancers combine output pairs.

**Correctness**: Lemma 12.5.6 — if inputs have the step property, outputs
do too.

**Application**: Core correctness component of the bitonic network.  Also
used in bitonic sorting networks and distributed merge operations.

---

### 3. Bitonic[w] (Herlihy Fig 12.13, 12.16, Theorem 12.5.1)

**What**: Full counting network.  Two half-width Bitonic networks feed
into a full-width Merger.

**Construction**: Bitonic[2k] = Bitonic[k] ∘ Bitonic[k] → Merger[2k]
Base: Bitonic[2] = single balancer.

**Step Property**: In any quiescent state with m total tokens,
yᵢ = ⌈(m-i)/w⌉.  Tokens distribute cyclically: 0, 1, …, w-1, 0, 1, …

**Application**:
- **Distributed counters** (Fig 12.9): w output wires each maintain a
  local counter.  Thread gets unique index = wire + round × w.
- **Memory pool allocators**: disjoint slot sets per wire.
- **Work-stealing schedulers**: balanced task distribution.
- **Barrier synchronisation**: distributed wakeup traffic.

---

### 4. Block[w] (Herlihy Fig 12.19)

**What**: Building block for periodic networks.  Splits input into top/bottom
halves, routes through sub-blocks, combines with final balancers.

**Construction**: Block[2k] = Block[k] (top) + Block[k] (bottom) + k
final balancers.  Base: Block[2] = single balancer.

**Application**:
- **Hardware (FPGA/ASIC)**: Regular structure minimises routing complexity.
- **Pipeline stages**: Each block is one pipeline stage for overlapped
  token processing.
- **Fault tolerance**: Identical blocks can be hot-swapped.

---

### 5. Periodic[w] (Herlihy Fig 12.18)

**What**: A counting network made of log(k) identical Block[w] stages
connected in sequence.  Every stage is structurally identical — the
network is *periodic*.

**Construction**: Periodic[2k] = Block[2k]^(log k).

**Step Property**: Same as Bitonic[w] — achieved through iterative
refinement across repeated blocks.

**Application**:
- **VLSI layouts**: Periodicity maps to regular silicon floor plans.
- **Pipeline-parallel counters**: Each block is a pipeline stage.
- **Streaming systems**: Uniform timing analysis across stages.
- **Self-similar fault tolerance**: Any block is replaceable.

---

## Hardware Abstraction & ISA Support

The key requirement from Herlihy: **the traverse operation on each balancer
must be serialised**.  This can be achieved through:

### A. CAS / RMW Atomics (lock-free)
- **ISAs**: x86 (LOCK prefix), ARM (LDXR/STXR), RISC-V (LR/SC)
- **Mechanism**: `atomic::fetch_add` on the balancer counter
- **Trade-off**: Best throughput, requires hardware RMW support
- **Lock names**: `bitonic_cas`, `periodic_cas`

### B. Software Mutex — Burns-Lamport
- **ISAs**: Any with loads, stores, and memory fences (x86, ARM, RISC-V, SPARC, MIPS)
- **Mechanism**: Per-balancer Burns-Lamport mutual exclusion (N-thread safe)
- **Trade-off**: No RMW needed, but O(n) doorway per balancer access
- **Lock names**: `bitonic_bl`, `periodic_bl`

### C. Software Mutex — Lamport Fast Lock
- **ISAs**: Same as Burns-Lamport
- **Mechanism**: Per-balancer Lamport fast lock (O(1) uncontended fast path)
- **Trade-off**: Good for low contention; degrades to O(n) under contention
- **Lock names**: `bitonic_lamport`, `periodic_lamport`

### D. Elevator Lock (Buhr, Dice, Scherer 2005)
- **ISAs**: Requires single CAS for enqueue (can be replaced with LL/SC)
- **Mechanism**: MCS-style queue lock per balancer, local spinning
- **Trade-off**: Ideal for NUMA/CXL — each thread spins on its own node,
  only the handoff write crosses NUMA domains.  Sequential handoff
  ("elevator" property) provides FIFO ordering.
- **Lock names**: `bitonic_elevator`, `periodic_elevator`

### E. Bakery Algorithm (Lamport, no atomics)
- **ISAs**: Any with sequential consistency (or fence-augmented TSO/ARM)
- **Mechanism**: Lamport's bakery algorithm per balancer — pure loads & stores
- **Trade-off**: Most portable, but O(n) space and time per balancer access.
  Suitable for research/embedded ISAs without any atomic RMW.
- **Lock names**: `bitonic_bakery`, `periodic_bakery`

---

## Memory Model Considerations

| ISA | Memory Model | Recommended Sync | Notes |
|---|---|---|---|
| x86-64 | TSO (strong) | CAS or Burns-Lamport | `LOCK` prefix for CAS; stores are ordered |
| ARM (v8+) | Weakly ordered | CAS or Lamport | Requires `DMB ISH` fences; `LDXR/STXR` for CAS |
| RISC-V | RVWMO (weak) | CAS or Bakery | `LR/SC` for CAS; `fence` for software locks |
| SPARC (TSO mode) | TSO | Burns-Lamport | Similar to x86; `MEMBAR` for fences |
| MIPS | Weakly ordered | Bakery or Lamport | `LL/SC` available on most; `SYNC` for fences |
| Embedded (no RMW) | Varies | Bakery | Pure load/store algorithms only |

---

## File Map

| File | Contents |
|---|---|
| `lib/lock/bitonic_networks.hpp` | All network + lock implementations |
| `lib/utils/bench_utils.cpp` | Factory registration (both CXL and standard paths) |
| `BITONIC_NETWORKS_COMPLEXITY.md` | This document |

## Lock Name Registry

```
bitonic_cas        bitonic_bl         bitonic_lamport
bitonic_elevator   bitonic_bakery
periodic_cas       periodic_bl        periodic_lamport
periodic_elevator  periodic_bakery
```
