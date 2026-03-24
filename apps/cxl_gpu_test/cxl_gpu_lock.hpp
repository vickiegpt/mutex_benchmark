#ifndef CXL_GPU_LOCK_HPP
#define CXL_GPU_LOCK_HPP

#pragma once

#include <atomic>
#include <cstdint>
#include "../../lib/utils/cxl_utils.hpp"

// CPU-side ticket lock functions using std::atomic

inline uint64_t cpu_ticket_acquire(std::atomic_uint64_t* now_serving, std::atomic_uint64_t* next_ticket) {
    // Atomically get next ticket (PCIe FetchAdd TLP to Completer)
    uint64_t my_ticket = next_ticket->fetch_add(1, std::memory_order_relaxed);

    // Poll until our ticket is now being served
    while (true) {
        // INVALIDATE: bypass host cache, force read from CXL device memory
        INVALIDATE(now_serving);

        // Load with acquire semantics
        uint64_t current_serving = now_serving->load(std::memory_order_acquire);

        if (current_serving == my_ticket) {
            break;  // Our turn to enter critical section
        }

        // Busy-wait to reduce bus traffic
        #ifdef __CUDA__
            // In CUDA, just yield
            __nop();
        #else
            // In C++, use PAUSE instruction
            asm volatile("pause" ::: "memory");
        #endif
    }

    return my_ticket;
}

inline void cpu_ticket_release(std::atomic_uint64_t* now_serving) {
    // Atomically increment now_serving to release lock to next waiter
    now_serving->fetch_add(1, std::memory_order_release);

    // FLUSH: push the updated counter to CXL device memory
    FLUSH(now_serving);

    // Full fence to ensure visibility - use inline asm in GPU code
    #ifdef __CUDA__
        __threadfence_system();
    #else
        #if defined(__x86_64)
            asm volatile("lock; addq $0,128(%%rsp);" ::: "cc");
        #else
            #error "Unsupported architecture"
        #endif
    #endif
}

// GPU-side ticket lock functions (defined in .cu files)
// These are __device__ functions that use CUDA atomics and fences

// Forward declarations (actual implementations in cxl_gpu_kernels.cu)
#ifdef __CUDACC__

__device__ inline uint64_t gpu_ticket_acquire(unsigned long long* now_serving, unsigned long long* next_ticket) {
    // GPU atomicAdd_system: system-scope atomic that generates PCIe AtomicOp TLP
    // SM 7.0+ supports atomicAdd_system; older GPUs fall back to regular atomicAdd
    #if __CUDA_ARCH__ >= 700
        uint64_t my_ticket = atomicAdd_system(next_ticket, 1ULL);
    #else
        uint64_t my_ticket = atomicAdd((unsigned long long*)next_ticket, 1ULL);
    #endif

    // Poll until our ticket is being served
    while (true) {
        // Full system fence to ensure cross-PCIe visibility of writes
        __threadfence_system();

        // Atomic read (0-valued atomicAdd for non-destructive read)
        #if __CUDA_ARCH__ >= 700
            uint64_t current = atomicAdd_system((unsigned long long*)now_serving, 0ULL);
        #else
            uint64_t current = atomicAdd((unsigned long long*)now_serving, 0ULL);
        #endif

        if (current == my_ticket) {
            break;
        }

        // Busy-wait - just continue spinning
    }

    return my_ticket;
}

__device__ inline void gpu_ticket_release(unsigned long long* now_serving) {
    // Ensure all critical section writes are visible before releasing
    __threadfence_system();

    // Atomically increment now_serving
    #if __CUDA_ARCH__ >= 700
        atomicAdd_system((unsigned long long*)now_serving, 1ULL);
    #else
        atomicAdd((unsigned long long*)now_serving, 1ULL);
    #endif

    // Ensure the release write propagates
    __threadfence_system();
}

#endif  // __CUDACC__

#endif  // CXL_GPU_LOCK_HPP
