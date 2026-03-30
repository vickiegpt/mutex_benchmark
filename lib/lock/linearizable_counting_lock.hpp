#ifndef LINEARIZABLE_COUNTING_LOCK_HPP
#define LINEARIZABLE_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "../utils/cxl_utils.hpp"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <new>
#include <atomic>
#include <algorithm>
#include <string>

// =============================================================================
// Linearizable-Window Counting Locks
//
// Two lock designs that exploit counting-network structure for successor
// lookup, reducing unlock cost from O(n) to O(W) or O(1).
//
// Based on:
//   [1] Lynch, Shavit, Shvartsman (1996) — "Counting Networks Are Practically
//       Linearizable"  (PODC 96)
//   [2] Herlihy, Shavit, Waarts (1996) — "Linearizable Counting Networks"
//       (Distributed Computing)
//
// See LINEARIZABLE_COUNTING_ANALYSIS.md for full theoretical analysis.
//
// ═════════════════════════════════════════════════════════════════════════════
//
// DESIGN A — Wire-Indexed Counting Lock  (WireIndexedBitonicLock, etc.)
//
//   Uses per-wire slot arrays indexed by per-pair round.  Unlock predicts
//   the successor's (wire, round) from the step property and checks ONE
//   slot.  Falls back to O(W) wire sweep if prediction misses.
//
//   Lock path:  O(log²W) balancer traversals  (same as standard)
//   Unlock:     O(1) predicted / O(W) fallback
//   Extra cost: NONE — no additional atomic on lock path
//
//   Assumptions:
//     A1  Step property approximately holds under concurrency
//     A2  Per-pair rounds monotonically increase per wire (guaranteed by
//         linearizable balancers: each balancer uses fetch_add)
//     A5  Per-pair rounds across final-layer balancers are INDEPENDENT —
//         prediction may miss when step property is violated
//
// DESIGN B — Sequenced Counting Lock  (SeqBitonicLock, etc.)
//
//   Adds ONE global fetch_add after network traversal.  The sequence number
//   is the linearization point — tokens are contiguous and globally ordered.
//   Unlock checks slot[seq + 1] for guaranteed O(1).
//
//   Lock path:  O(log²W) traversal + 1 fetch_add  (one extra atomic)
//   Unlock:     O(1) guaranteed
//   Extra cost: One shared cache line (global_seq_)
//
//   The counting network distributes arrival times across W/2 balancers,
//   so threads hit global_seq_ at staggered intervals — less contention
//   than a bare ticket lock.
// =============================================================================
// DESIGN C: Waiting-Filter Counting Lock  (Design C)
//
// From Herlihy, Shavit, Waarts (1996), Section 3: "The Waiting Network"
//
// A counting network + n-element phase-bit array that linearizes the output.
// Each token v waits for predecessor v-1 to set its phase bit (at unlock),
// then enters CS.  Unlock sets own phase bit — O(1) always.
//
// Key insight from Lynch, Shavit, Shvartsman (1996):
//   Under practical linearizability (c2 ≤ 2·c1, i.e., bounded timing
//   variation on wires), predecessors have ALREADY completed before
//   successors check, so the phase-bit wait is near-zero in practice.
//   The Waiting-filter is still needed for correctness under all timings
//   (preemption, NUMA/CXL with c2 >> c1).
//
// Complexity:
//   Lock:   O(log²W) network traverse + O(1) phase check (practical)
//           O(log²W) + O(chain) phase check (worst case, chain ≤ n)
//   Unlock: O(1) always — set one phase bit
//   Space:  O(n·CL) phase bits + O(n·CL) per-thread values
//   Extra atomics on lock path: NONE
//
// phase(v) = ⌊v/n⌋ mod 2   — toggles every n values per slot, preventing ABA
// =============================================================================
//
// =============================================================================


// ─── Spin-wait hint ──────────────────────────────────────────────────────────
#if defined(__aarch64__) || defined(_M_ARM64)
  #define LcSpinHint() __asm__ volatile("yield")
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define LcSpinHint() __asm__ volatile("pause")
#else
  #define LcSpinHint() ((void)0)
#endif


// =============================================================================
// SECTION 1: Wire-Indexed Counting Lock  (Design A)
//
// Per-wire slot arrays enable O(1) predicted / O(W) fallback unlock.
// =============================================================================

template <typename Sync, template <typename> class NetworkT>
class WireIndexedCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        // Width = smallest power of 2 >= ceil(sqrt(n)), minimum 2
        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        // Per-wire capacity: enough for all concurrent waiters per wire + margin
        rounds_cap_ = std::max((size_t)16, 4 * num_threads / width_ + 4);

        network_.build(width_, num_threads);

        // ── Memory layout (single ALLOCATE region) ──
        // [waker_flag : CL] [wire_heads : W*CL] [wire_slots : W*rounds_cap*CL]
        // [thread_meta : n*CL] [waker_lock_mem : n*bool]
        size_t waker_flag_bytes = CL;
        size_t wire_heads_bytes = width_ * CL;
        size_t wire_slots_bytes = width_ * rounds_cap_ * CL;
        size_t meta_bytes       = num_threads * CL;
        size_t waker_bytes      = sizeof(volatile bool) * num_threads;

        region_size_ = waker_flag_bytes + wire_heads_bytes +
                       wire_slots_bytes + meta_bytes + waker_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        waker_flag_      = (volatile bool*)&region_[off];      off += waker_flag_bytes;
        wire_heads_base_ = &region_[off];                      off += wire_heads_bytes;
        wire_slots_base_ = &region_[off];                      off += wire_slots_bytes;
        meta_base_       = &region_[off];                      off += meta_bytes;
        volatile bool* waker_mem = (volatile bool*)&region_[off];

        // Init wire heads to round 0
        for (size_t w = 0; w < width_; w++) {
            get_wire_head(w)->next_round = 0;
        }

        // Init all wire slots as unoccupied
        for (size_t w = 0; w < width_; w++) {
            for (size_t r = 0; r < rounds_cap_; r++) {
                auto* s = get_wire_slot(w, r);
                s->occupied = false;
                s->tid = 0;
                s->round = 0;
                s->spin_addr = nullptr;
            }
        }

        *waker_flag_ = true;   // Lock starts free
        next_to_serve_ = 0;    // Global step-property counter
        waker_lock_.init(waker_mem, num_threads);
        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        volatile bool local_grant = false;

        // 1. Traverse counting network → (wire, per-pair round)
        size_t lv = 0;
        int wire_i = network_.traverse((int)(thread_id % width_),
                                       thread_id, &lv);
        size_t wire  = (size_t)wire_i;
        size_t round = lv >> 1;

        // Store for unlock
        auto* meta = get_meta(thread_id);
        meta->wire  = wire;
        meta->round = round;

        // 2. Register in per-wire slot
        auto* slot = get_wire_slot(wire, round % rounds_cap_);
        slot->tid       = thread_id;
        slot->round     = round;
        slot->spin_addr = &local_grant;
        Fence();
        slot->occupied = true;
        Fence();

        // 3. Designated-waker protocol
        if (waker_lock_.trylock(thread_id)) {
            Fence();
            while (!local_grant && !*waker_flag_) { LcSpinHint(); }
            *waker_flag_ = false;
            Fence();
            waker_lock_.unlock();
            // Entered via waker flag: clear our slot and advance wire head
            // so unlock() sweep doesn't miss future waiters on this wire.
            if (!local_grant) {
                slot->occupied = false;
                // Advance wire_heads past our round (we're self-served)
                auto* head = get_wire_head(wire);
                if (head->next_round <= round)
                    head->next_round = round + 1;
            }
        } else {
            while (!local_grant) { LcSpinHint(); }
        }
    }

    void unlock(size_t /*thread_id*/) override {
        // Step-property prediction:
        //   Global token ordering: wire 0 r0, wire 1 r0, ..., wire W-1 r0,
        //                          wire 0 r1, wire 1 r1, ...
        //   next_to_serve_ tracks position in this ordering.
        size_t next = next_to_serve_;
        next_to_serve_ = next + 1;

        size_t pred_wire  = next % width_;
        // Use wire_heads for the actual per-wire round to serve
        size_t pred_round = get_wire_head(pred_wire)->next_round;

        // First: check predicted (wire, round) — O(1)
        auto* slot = get_wire_slot(pred_wire, pred_round % rounds_cap_);
        if (slot->occupied && slot->round == pred_round) {
            get_wire_head(pred_wire)->next_round = pred_round + 1;
            *slot->spin_addr = true;
            Fence();
            slot->occupied = false;
            return;
        }

        // Prediction missed — sweep all W wires for the minimum
        // (round, wire_distance) waiter.  This handles step-property
        // violations under concurrency.

        size_t best_wire  = width_;
        size_t best_round = ~(size_t)0;
        WireSlot* best_slot = nullptr;

        for (size_t w = 0; w < width_; w++) {
            size_t r = get_wire_head(w)->next_round;
            auto* s = get_wire_slot(w, r % rounds_cap_);

            // Prefetch next wire's head
            if (w + 1 < width_)
                __builtin_prefetch(get_wire_head(w + 1), 0, 1);

            if (s->occupied && s->round == r) {
                // Compare (round, wire_dist) lexicographically
                size_t wd = (w + width_ - pred_wire) % width_;
                if (r < best_round ||
                    (r == best_round && wd < ((best_wire + width_ - pred_wire) % width_))) {
                    best_round = r;
                    best_wire  = w;
                    best_slot  = s;
                }
            }
        }

        if (best_slot) {
            get_wire_head(best_wire)->next_round = best_round + 1;
            *best_slot->spin_addr = true;
            Fence();
            best_slot->occupied = false;
            return;
        }

        // No waiter found — set waker flag for late arrivals
        *waker_flag_ = true;
        Fence();
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        if (region_) {
            FREE((void*)region_, region_size_);
            region_ = nullptr;
        }
    }

    std::string name() override {
        std::string net = "bitonic";
        // Detect periodic by checking if NetworkT is PeriodicNetwork
        // (template trick: check build signature)
        return std::string("lw_") + net + "_" + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;

    // Per-wire slot: one waiter registration at a (wire, round) position
    struct WireSlot {
        volatile bool   occupied;
        volatile size_t tid;
        volatile size_t round;      // For verification against wrap-around
        volatile bool*  spin_addr;
    };

    // Per-wire head: tracks the next round to serve on this wire
    struct WireHead {
        size_t next_round;
    };

    // Per-thread metadata: holder's (wire, round) for unlock
    struct ThreadMeta {
        size_t wire;
        size_t round;
    };

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;
    size_t rounds_cap_  = 0;

    volatile char*  region_     = nullptr;
    size_t          region_size_ = 0;
    volatile bool*  waker_flag_ = nullptr;
    volatile char*  wire_heads_base_ = nullptr;
    volatile char*  wire_slots_base_ = nullptr;
    volatile char*  meta_base_  = nullptr;

    BnWakerLock     waker_lock_;
    size_t          next_to_serve_ = 0;   // Protected by lock ownership
    bool            initialized_ = false;

    // CL-padded accessors
    WireHead* get_wire_head(size_t wire) const {
        return (WireHead*)&wire_heads_base_[wire * CL];
    }

    WireSlot* get_wire_slot(size_t wire, size_t slot_idx) const {
        return (WireSlot*)&wire_slots_base_[(wire * rounds_cap_ + slot_idx) * CL];
    }

    ThreadMeta* get_meta(size_t tid) const {
        return (ThreadMeta*)&meta_base_[tid * CL];
    }
};

// Specialization for name() — detect network type at compile time
// (We use partial specialization via a helper)

template <typename Sync>
class WireIndexedBitonicLock
    : public WireIndexedCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("lw_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class WireIndexedPeriodicLock
    : public WireIndexedCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("lw_periodic_") + Sync::sync_name();
    }
};


// =============================================================================
// SECTION 2: Sequenced Counting Lock  (Design B)
//
// Global fetch_add provides contiguous token ordering.
// Guaranteed O(1) unlock via slot array indexed by sequence number.
// =============================================================================

template <typename Sync, template <typename> class NetworkT>
class SequencedCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        // Width for network: power of 2 >= ceil(sqrt(n))
        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        // Slot array: power of 2, at least 4x threads for wrap safety
        num_slots_ = 1;
        while (num_slots_ < 4 * num_threads) num_slots_ *= 2;
        slot_mask_ = num_slots_ - 1;

        network_.build(width_, num_threads);

        // ── Memory layout ──
        // [waker_flag : CL] [now_serving : CL] [slot_entries : num_slots*CL]
        // [padding : CL]
        size_t pad_bytes         = CL;
        size_t now_serving_bytes = CL;
        size_t slot_bytes        = num_slots_ * CL;

        region_size_ = pad_bytes + now_serving_bytes + slot_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        off += pad_bytes;  // padding / alignment
        now_serving_  = (volatile size_t*)&region_[off];  off += now_serving_bytes;
        slot_base_    = &region_[off];

        global_seq_.store(0, std::memory_order_relaxed);
        *now_serving_ = 0;

        for (size_t i = 0; i < num_slots_; i++) {
            auto* s = get_slot(i);
            s->occupied = false;
            s->token    = 0;
            s->spin_addr = nullptr;
        }

        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        volatile bool local_grant = false;

        // 1. Traverse counting network (distributes contention)
        //    The network output is NOT used for ordering — only for
        //    distributing the contention across W/2 balancers so that
        //    threads arrive at global_seq_ at staggered intervals.
        network_.traverse((int)(thread_id % width_), thread_id);

        // 2. Linearization point: global sequence number
        //    This single fetch_add produces contiguous, globally-ordered tokens.
        size_t seq = global_seq_.fetch_add(1, std::memory_order_acq_rel);

        // 3. Register in slot array for direct handoff
        size_t my_slot = seq & slot_mask_;
        auto* slot = get_slot(my_slot);
        slot->spin_addr = &local_grant;
        slot->token     = seq;
        Fence();
        slot->occupied  = true;
        Fence();

        // 4. Spin until it's our turn (via now_serving or direct handoff)
        while (!local_grant && *now_serving_ != seq) {
            LcSpinHint();
        }
    }

    void unlock(size_t /*thread_id*/) override {
        // Clear current slot
        size_t cur = *now_serving_;
        get_slot(cur & slot_mask_)->occupied = false;
        Fence();

        // Advance to next token
        size_t next_token = cur + 1;
        *now_serving_ = next_token;
        Fence();

        // Try to wake successor directly — O(1) handoff
        size_t next_slot_idx = next_token & slot_mask_;
        auto* nse = get_slot(next_slot_idx);

        static constexpr int HANDOFF_SPINS = 64;
        for (int i = 0; i < HANDOFF_SPINS; i++) {
            if (nse->occupied && nse->token == next_token) {
                *nse->spin_addr = true;
                Fence();
                return;
            }
            LcSpinHint();
        }
        // Successor will see now_serving_ update and proceed
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        if (region_) {
            FREE((void*)region_, region_size_);
            region_ = nullptr;
        }
    }

    std::string name() override {
        return std::string("seq_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;

    struct SlotEntry {
        volatile bool   occupied;
        volatile size_t token;
        volatile bool*  spin_addr;
    };

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;
    size_t num_slots_   = 0;
    size_t slot_mask_   = 0;

    std::atomic<size_t> global_seq_{0};   // The linearization point

    volatile char*  region_      = nullptr;
    size_t          region_size_ = 0;
    volatile size_t* now_serving_ = nullptr;
    volatile char*  slot_base_   = nullptr;

    bool            initialized_ = false;

    SlotEntry* get_slot(size_t idx) const {
        return (SlotEntry*)&slot_base_[idx * CL];
    }
};

// Named specializations per network type

template <typename Sync>
class SeqBitonicLock
    : public SequencedCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("seq_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class SeqPeriodicLock
    : public SequencedCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("seq_periodic_") + Sync::sync_name();
    }
};


// =============================================================================
// SECTION 3: Waiting-Filter Counting Lock  (Design C)
//
// From Herlihy, Shavit, Waarts (1996), Section 3: "The Waiting Network"
//
// A counting network + n-element phase-bit array that linearizes the output.
// Each token v waits for predecessor v-1 to set its phase bit (at unlock),
// then enters CS.  Unlock sets own phase bit — O(1) always.
//
// Key insight from Lynch, Shavit, Shvartsman (1996):
//   Under practical linearizability (c2 ≤ 2·c1, i.e., bounded timing
//   variation on wires), predecessors have ALREADY completed before
//   successors check, so the phase-bit wait is near-zero in practice.
//   The Waiting-filter is still needed for correctness under all timings
//   (preemption, NUMA/CXL with c2 >> c1).
//
// Complexity:
//   Lock:   O(log²W) network traverse + O(1) phase check (practical)
//           O(log²W) + O(chain) phase check (worst case, chain ≤ n)
//   Unlock: O(1) always — set one phase bit
//   Space:  O(n·CL) phase bits + O(n·CL) per-thread values
//   Extra atomics on lock path: NONE
//
// phase(v) = ⌊v/n⌋ mod 2   — toggles every n values per slot, preventing ABA
// =============================================================================

template <typename Sync, template <typename> class NetworkT>
class WaitingFilterCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        network_.build(width_, num_threads);

        // ── Memory layout ──
        // [phase_bits : n * CL]  [thread_values : n * CL]
        size_t phase_bytes = num_threads * CL;
        size_t value_bytes = num_threads * CL;

        region_size_ = phase_bytes + value_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        phase_base_ = &region_[off]; off += phase_bytes;
        value_base_ = &region_[off];

        // Init phase_bits to PHASE_UNSET sentinel (0xFF).
        // This is neither 0 nor 1, so no token can accidentally match
        // a predecessor's phase before that predecessor has actually unlocked.
        // Token 0 is special-cased to not wait (no predecessor).
        for (size_t i = 0; i < num_threads; i++) {
            *get_phase_bit(i) = PHASE_UNSET;
            *get_thread_value(i) = 0;
        }

        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        // 1. Traverse counting network → (wire, round) → global token v
        //    Token value v = round * width + wire  (contiguous at quiescence
        //    by the step property;  unique under concurrency)
        size_t lv = 0;
        int wire_i = network_.traverse((int)(thread_id % width_),
                                       thread_id, &lv);
        size_t wire  = (size_t)wire_i;
        size_t round = lv >> 1;
        size_t v = round * width_ + wire;

        *get_thread_value(thread_id) = v;

        // 2. Waiting-filter (Herlihy-Shavit-Waarts Section 3)
        //    Wait for predecessor (value v-1) to set its phase bit.
        //    Token 0 has no predecessor → enters immediately.
        //
        //    Under practical linearizability (Lynch et al., c2 ≤ 2c1):
        //    predecessor has almost always ALREADY set its phase bit
        //    before we check, so this spin is near-zero in practice.
        if (v > 0) {
            size_t pred_slot = (v - 1) % num_threads_;
            uint8_t expected = phase_of(v - 1);
            while (*get_phase_bit(pred_slot) != expected) {
                LcSpinHint();
            }
        }
        // Lock acquired.
    }

    void unlock(size_t thread_id) override {
        // O(1): set own phase bit to signal successor.
        // Successor (value v+1) will see this and proceed.
        size_t v = *get_thread_value(thread_id);
        size_t my_slot = v % num_threads_;
        *get_phase_bit(my_slot) = phase_of(v);
        Fence();
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        if (region_) {
            FREE((void*)region_, region_size_);
            region_ = nullptr;
        }
    }

    std::string name() override {
        return std::string("wf_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;
    static constexpr uint8_t PHASE_UNSET = 0xFF;

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;

    volatile char*  region_     = nullptr;
    size_t          region_size_ = 0;
    volatile char*  phase_base_ = nullptr;
    volatile char*  value_base_ = nullptr;

    bool            initialized_ = false;

    // phase(v) = ⌊v/n⌋ mod 2  (Herlihy-Shavit-Waarts)
    // Toggles every n values per slot, preventing ABA on the circular buffer.
    uint8_t phase_of(size_t v) const {
        return (uint8_t)((v / num_threads_) % 2);
    }

    volatile uint8_t* get_phase_bit(size_t slot) const {
        return (volatile uint8_t*)&phase_base_[slot * CL];
    }

    volatile size_t* get_thread_value(size_t tid) const {
        return (volatile size_t*)&value_base_[tid * CL];
    }
};

// Named specializations per network type

template <typename Sync>
class WFBitonicLock
    : public WaitingFilterCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("wf_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class WFPeriodicLock
    : public WaitingFilterCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("wf_periodic_") + Sync::sync_name();
    }
};


// =============================================================================
// SECTION 4: Concrete Type Aliases
// =============================================================================

//TODO: DOES NOT WORK
// ── Design A: Wire-Indexed (predictive O(1)/O(W) unlock) ────────────────────
using LWBitonicCASLock      = WireIndexedBitonicLock<BnCASSync>;
using LWBitonicBLLock       = WireIndexedBitonicLock<BnBLSync>;
using LWBitonicLamportLock  = WireIndexedBitonicLock<BnLamportSync>;
using LWBitonicBakeryLock   = WireIndexedBitonicLock<BnBakerySync>;

using LWPeriodicCASLock      = WireIndexedPeriodicLock<BnCASSync>;
using LWPeriodicBLLock       = WireIndexedPeriodicLock<BnBLSync>;
using LWPeriodicLamportLock  = WireIndexedPeriodicLock<BnLamportSync>;
using LWPeriodicBakeryLock   = WireIndexedPeriodicLock<BnBakerySync>;

//TODO: DOES NOT WORK
// ── Design B: Sequenced (guaranteed O(1) unlock) ────────────────────────────
using SeqBitonicCASLock      = SeqBitonicLock<BnCASSync>;
using SeqBitonicBLLock       = SeqBitonicLock<BnBLSync>;
using SeqBitonicLamportLock  = SeqBitonicLock<BnLamportSync>;
using SeqBitonicBakeryLock   = SeqBitonicLock<BnBakerySync>;

using SeqPeriodicCASLock      = SeqPeriodicLock<BnCASSync>;
using SeqPeriodicBLLock       = SeqPeriodicLock<BnBLSync>;
using SeqPeriodicLamportLock  = SeqPeriodicLock<BnLamportSync>;
using SeqPeriodicBakeryLock   = SeqPeriodicLock<BnBakerySync>;

// ── Design C: Waiting-Filter (O(1) unlock, phase-bit chain) ─────────────────
using WFBitonicCASLock      = WFBitonicLock<BnCASSync>;
using WFBitonicBLLock       = WFBitonicLock<BnBLSync>;
using WFBitonicLamportLock  = WFBitonicLock<BnLamportSync>;
using WFBitonicBakeryLock   = WFBitonicLock<BnBakerySync>;


using WFPeriodicCASLock      = WFPeriodicLock<BnCASSync>;
using WFPeriodicBLLock       = WFPeriodicLock<BnBLSync>;
using WFPeriodicLamportLock  = WFPeriodicLock<BnLamportSync>;
using WFPeriodicBakeryLock   = WFPeriodicLock<BnBakerySync>;

#endif // LINEARIZABLE_COUNTING_LOCK_HPP
