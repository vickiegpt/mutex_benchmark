#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>
#include "cxl_gpu_lock.hpp"

// CUDA Kernels for CPU-GPU CXL memory access testing

// Kernel 1: FetchAdd Total Ordering Test
// =======================================
// N GPU threads each do K atomicAdd operations on a shared counter
// Each thread records the returned value (the pre-increment counter value)
// These values should form a total order: {0, 1, 2, ..., N*K-1}
__global__ void kernel_fetchadd_order(unsigned long long* shared_counter,
                                       unsigned long long* out_values,
                                       int iterations_per_thread) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;

    for (int i = 0; i < iterations_per_thread; i++) {
        // Atomic increment, get the pre-increment value
        #if __CUDA_ARCH__ >= 700
            uint64_t val = atomicAdd_system(shared_counter, 1ULL);
        #else
            uint64_t val = atomicAdd(shared_counter, 1ULL);
        #endif

        // Record this value
        int global_idx = tid * iterations_per_thread + i;
        out_values[global_idx] = val;
    }
}

// Kernel 2: Dekker's Ordering Test (GPU side - Thread B)
// ======================================================
// Thread B (GPU): write y=1, fence, read x
// Records what value of x was observed
// Should run concurrently with CPU Thread A: write x=1, fence, read y
__global__ void kernel_dekker_b(unsigned long long* x,
                                 unsigned long long* y,
                                 unsigned long long* gpu_saw_x_values,
                                 int iterations) {
    // Only one GPU thread runs this
    if (blockIdx.x != 0 || threadIdx.x != 0) return;

    for (int i = 0; i < iterations; i++) {
        // Write y = 1 with system-scope atomicity
        #if __CUDA_ARCH__ >= 700
            atomicAdd_system((unsigned long long*)y, 1ULL);
        #else
            *y = 1;
            __threadfence_system();
        #endif

        // System-wide fence
        __threadfence_system();

        // Read x - what did Thread A write?
        uint64_t saw_x = *x;

        // Store result
        gpu_saw_x_values[i] = saw_x;

        // Reset for next iteration (CPU side will also reset x and y)
        // Coordinate via a shared flag if needed, but for simplicity
        // CPU handles the reset between iterations
    }
}

// Kernel 3: Ticket Lock Contention Test
// ======================================
// GPU threads contend on a shared ticket lock in CXL memory
// Each thread acquires lock, increments counter, releases lock
__global__ void kernel_ticket_contention(unsigned long long* now_serving,
                                          unsigned long long* next_ticket,
                                          unsigned long long* counter,
                                          int iterations_per_thread) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;

    for (int i = 0; i < iterations_per_thread; i++) {
        // Acquire ticket lock
        uint64_t my_ticket = gpu_ticket_acquire(now_serving, next_ticket);

        // Critical section: increment shared counter
        // This should be protected by the lock, so no atomics needed here
        __threadfence_system();  // Ensure lock was truly acquired
        *counter = *counter + 1;
        __threadfence_system();

        // Release lock
        gpu_ticket_release(now_serving);
    }
}

// Host-callable wrapper functions
// ================================

extern "C" {

cudaError_t launch_fetchadd_order(cudaStream_t stream,
                                   unsigned long long* dev_counter,
                                   unsigned long long* dev_values,
                                   int n_threads,
                                   int iterations) {
    int threads_per_block = 128;
    int blocks = (n_threads + threads_per_block - 1) / threads_per_block;

    kernel_fetchadd_order<<<blocks, threads_per_block, 0, stream>>>(
        dev_counter, dev_values, iterations);

    return cudaGetLastError();
}

cudaError_t launch_dekker_b(cudaStream_t stream,
                             unsigned long long* dev_x,
                             unsigned long long* dev_y,
                             unsigned long long* dev_out,
                             int iterations) {
    // Run with 1 thread total (1 block, 1 thread per block)
    kernel_dekker_b<<<1, 1, 0, stream>>>(dev_x, dev_y, dev_out, iterations);
    return cudaGetLastError();
}

cudaError_t launch_ticket_contention(cudaStream_t stream,
                                      unsigned long long* dev_ns,
                                      unsigned long long* dev_nt,
                                      unsigned long long* dev_counter,
                                      int n_threads,
                                      int iterations) {
    int threads_per_block = 128;
    int blocks = (n_threads + threads_per_block - 1) / threads_per_block;

    kernel_ticket_contention<<<blocks, threads_per_block, 0, stream>>>(
        dev_ns, dev_nt, dev_counter, iterations);

    return cudaGetLastError();
}

}  // extern "C"
