#ifndef CXL_TICKET_LOCK_HPP
#define CXL_TICKET_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <atomic>
#include <stdexcept>

// CXL 2.0 Switch-Safe Ticket Spinlock
//
// Correctness argument for CXL 2.0 switch topology:
//
// 1. Per-Requester FetchAdd serialization:
//    - fetch_add(next_ticket, 1) is a PCIe FetchAdd TLP (equivalent to LOCK XADD on x86)
//    - All N agents requesting tickets are totally ordered at the single Completer (CXL device memory)
//    - Each agent receives a unique monotonically increasing ticket number
//
// 2. Per-location write visibility:
//    - Only the lock holder (thread with my_ticket == now_serving) writes now_serving
//    - FLUSH(now_serving) after unlock ensures all concurrent writes are serialized at the Completer
//    - No cross-Requester write serialization needed (only one writer at a time)
//
// 3. Cache bypass for polling:
//    - INVALIDATE(now_serving) before each load forces bypass of host L1/L2 cache
//    - Subsequent load reads directly from CXL device memory via uncacheable (UC) access or cache eviction
//    - Ensures polling threads see the latest value written by the lock holder
//
// 4. Ordering guarantee:
//    - After unlock's FLUSH + release semantics, the new now_serving value is visible to all polling agents
//    - Agents polling with INVALIDATE will transition from spinning to proceeding once now_serving changes
//
// This is the correct mutual exclusion primitive for CXL 2.0 switch multi-agent scenarios,
// where cross-agent write serialization is unavailable but per-location atomicity at the Completer is guaranteed.

class CXLTicketLock : public virtual SoftwareMutex {
public:
    static size_t get_cxl_region_size() {
        // Two cache-line-aligned atomic counters to avoid false sharing
        // now_serving placed first (more frequently polled)
        return std::hardware_destructive_interference_size * 2;
    }

    void init(size_t num_threads) override {
        (void)num_threads;  // Ticket lock is independent of thread count
        volatile char *_cxl_region = (volatile char *)ALLOCATE(get_cxl_region_size());
        region_init(_cxl_region);
    }

    void region_init(volatile char *_cxl_region) {
        this->_cxl_region = _cxl_region;
        // Layout: [now_serving (cache-line aligned)][next_ticket (cache-line aligned)]
        now_serving = (std::atomic_uint64_t *)&_cxl_region[0];
        next_ticket = (std::atomic_uint64_t *)&_cxl_region[std::hardware_destructive_interference_size];

        // Initialize to 0
        now_serving->store(0, std::memory_order_relaxed);
        next_ticket->store(0, std::memory_order_relaxed);
    }

    void lock(size_t thread_id) override {
        (void)thread_id;  // Ticket lock doesn't need thread_id for correctness

        // Acquire ticket: fetch_add is a PCIe FetchAdd TLP routed to the Completer
        // All N agents are totally ordered at this single location
        uint64_t my_ticket = next_ticket->fetch_add(1, std::memory_order_relaxed);

        // Poll until this thread's ticket is now being served
        while (true) {
            // INVALIDATE: bypass host cache, force load from CXL device memory
            INVALIDATE(now_serving);

            // Load with acquire semantics to ensure subsequent CS operations see lock-holder's writes
            uint64_t current_serving = now_serving->load(std::memory_order_acquire);

            if (current_serving == my_ticket) {
                // Our ticket is now being served, critical section acquired
                break;
            }

            // Back off with scheduler yield to reduce bus traffic during heavy contention
            spin_delay_sched_yield();
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;

        // Increment now_serving: let next ticket holder enter CS
        // fetch_add is atomic and uses release semantics for CS memory ordering
        now_serving->fetch_add(1, std::memory_order_release);

        // FLUSH: push the updated now_serving to CXL device memory
        // This ensures polling agents see the new value on their next INVALIDATE+load
        FLUSH(now_serving);

        // Full fence to ensure this is globally visible before unlock returns
        Fence();
    }

    void destroy() override {
        FREE((void *)_cxl_region, get_cxl_region_size());
    }

    std::string name() override {
        return "cxl_ticket";
    }

private:
    volatile char *_cxl_region;
    std::atomic_uint64_t *now_serving;
    std::atomic_uint64_t *next_ticket;
};

#endif  // CXL_TICKET_LOCK_HPP
