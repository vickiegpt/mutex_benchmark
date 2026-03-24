#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <vector>
#include <thread>
#include <set>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>

#include "../../lib/lock/lock.hpp"
#include "../../lib/utils/cxl_utils.hpp"
#include "../../lib/utils/cxl_type2_sysfs.hpp"
#include "cxl_gpu_lock.hpp"

#ifdef CUDA_GPU
#include <cuda_runtime.h>
extern "C" {
    cudaError_t launch_fetchadd_order(cudaStream_t stream,
                                       unsigned long long* dev_counter,
                                       unsigned long long* dev_values,
                                       int n_threads, int iterations);
    cudaError_t launch_dekker_b(cudaStream_t stream,
                                 unsigned long long* dev_x,
                                 unsigned long long* dev_y,
                                 unsigned long long* dev_out,
                                 int iterations);
    cudaError_t launch_ticket_contention(cudaStream_t stream,
                                          unsigned long long* dev_ns,
                                          unsigned long long* dev_nt,
                                          unsigned long long* dev_counter,
                                          int n_threads,
                                          int iterations);
}
#define CUDA_CHECK(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA error: %s\n", cudaGetErrorString(err)); \
        throw std::runtime_error(cudaGetErrorString(err)); \
    } \
} while(0)
#endif

struct TestResults {
    const char* test_name;
    bool pass;
    uint64_t violations;
    uint64_t total_ops;
    double throughput_ops_per_sec;
};

// Global test configuration
struct Config {
    bool use_cuda;
    bool use_type2;           // Use Type-2 CXL device instead of generic CXL
    int type2_device_id;      // 0 = cache0, 1 = cache1
    int cpu_threads;
    int gpu_threads;
    uint64_t iterations;
    bool csv_output;
    bool test_order;
    bool test_dekker;
    bool test_mutex;
    bool show_type2_info;     // Print Type-2 device info and exit
} g_config = {true, false, 0, 4, 4, 10000, false, true, true, true, false};

// ============================================================================
// Allocation helpers for Type-2 or generic CXL memory
// ============================================================================

static void* alloc_cxl_memory(size_t size) {
    if (g_config.use_type2) {
        return type2_sysfs_allocate(size, g_config.type2_device_id);
    } else {
        return ALLOCATE(size);
    }
}

static void free_cxl_memory(void* ptr, size_t size) {
    if (g_config.use_type2) {
        type2_sysfs_free(ptr, size);
    } else {
        FREE(ptr, size);
    }
}

// ============================================================================
// Test 1: FetchAdd Total Order Test
// ============================================================================

TestResults test_fetchadd_order() {
    TestResults result = {"FetchAdd Order", true, 0, 0, 0.0};

    // Allocate shared counter and result arrays
    unsigned long long* cxl_counter = (unsigned long long*)alloc_cxl_memory(sizeof(unsigned long long));
    *cxl_counter = 0;

    uint64_t total_threads = g_config.cpu_threads + g_config.gpu_threads;
    unsigned long long* all_values = (unsigned long long*)malloc(total_threads * g_config.iterations * sizeof(unsigned long long));

    auto start_time = std::chrono::high_resolution_clock::now();

#ifdef CUDA_GPU
    if (g_config.use_cuda) {
        // GPU side
        unsigned long long* dev_counter = nullptr;
        unsigned long long* dev_values = nullptr;

        CUDA_CHECK(cudaMalloc(&dev_counter, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&dev_values, total_threads * g_config.iterations * sizeof(unsigned long long)));

        CUDA_CHECK(cudaMemcpy(dev_counter, cxl_counter, sizeof(unsigned long long), cudaMemcpyHostToDevice));

        // Launch GPU kernel
        CUDA_CHECK(launch_fetchadd_order(0, dev_counter, dev_values, g_config.gpu_threads, g_config.iterations));

        // CPU threads do fetch_add concurrently
        std::vector<std::thread> cpu_threads;
        for (int t = 0; t < g_config.cpu_threads; t++) {
            cpu_threads.emplace_back([&](int thread_id) {
                for (uint64_t i = 0; i < g_config.iterations; i++) {
                    uint64_t val = std::atomic_ref(*cxl_counter).fetch_add(1, std::memory_order_relaxed);
                    all_values[thread_id * g_config.iterations + i] = val;
                }
            }, t);
        }

        // Wait for CPU threads
        for (auto& t : cpu_threads) t.join();

        // Wait for GPU
        CUDA_CHECK(cudaDeviceSynchronize());

        // Copy GPU results
        unsigned long long* gpu_values = (unsigned long long*)malloc(g_config.gpu_threads * g_config.iterations * sizeof(unsigned long long));
        CUDA_CHECK(cudaMemcpy(gpu_values, dev_values, g_config.gpu_threads * g_config.iterations * sizeof(unsigned long long), cudaMemcpyDeviceToHost));

        // Merge GPU results
        for (int t = 0; t < g_config.gpu_threads; t++) {
            for (uint64_t i = 0; i < g_config.iterations; i++) {
                all_values[g_config.cpu_threads * g_config.iterations + t * g_config.iterations + i] = gpu_values[t * g_config.iterations + i];
            }
        }

        free(gpu_values);
        CUDA_CHECK(cudaFree(dev_counter));
        CUDA_CHECK(cudaFree(dev_values));
    } else
#endif
    {
        // CPU-only mode (simulating GPU as separate threads)
        std::vector<std::thread> all_threads;

        // CPU threads
        for (int t = 0; t < g_config.cpu_threads; t++) {
            all_threads.emplace_back([&](int thread_id) {
                for (uint64_t i = 0; i < g_config.iterations; i++) {
                    uint64_t val = std::atomic_ref(*cxl_counter).fetch_add(1, std::memory_order_relaxed);
                    all_values[thread_id * g_config.iterations + i] = val;
                }
            }, t);
        }

        // "GPU" threads (simulated as CPU threads)
        for (int t = 0; t < g_config.gpu_threads; t++) {
            all_threads.emplace_back([&](int thread_id) {
                for (uint64_t i = 0; i < g_config.iterations; i++) {
                    uint64_t val = std::atomic_ref(*cxl_counter).fetch_add(1, std::memory_order_relaxed);
                    all_values[g_config.cpu_threads * g_config.iterations + thread_id * g_config.iterations + i] = val;
                }
            }, t);
        }

        for (auto& t : all_threads) t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    // Verify results: all values should be distinct and form {0, 1, ..., total*iterations-1}
    std::set<uint64_t> unique_values;
    result.total_ops = total_threads * g_config.iterations;

    for (uint64_t i = 0; i < result.total_ops; i++) {
        unique_values.insert(all_values[i]);
    }

    // Check for total order: all unique, no gaps
    if (unique_values.size() == result.total_ops) {
        uint64_t expected_max = result.total_ops - 1;
        if (*unique_values.rbegin() == expected_max) {
            result.pass = true;
            result.violations = 0;
        } else {
            result.pass = false;
            result.violations = 1;  // Unknown value range
        }
    } else {
        result.pass = false;
        result.violations = result.total_ops - unique_values.size();  // Count duplicates
    }

    // Calculate throughput
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    result.throughput_ops_per_sec = (result.total_ops * 1e6) / duration_us;

    free_cxl_memory(cxl_counter, sizeof(unsigned long long));
    free(all_values);

    return result;
}

// ============================================================================
// Test 2: Dekker's Cross-Device Ordering Test
// ============================================================================

TestResults test_dekker_ordering() {
    TestResults result = {"Dekker Order", true, 0, 0, 0.0};

    // Allocate x and y
    unsigned long long* x = (unsigned long long*)alloc_cxl_memory(sizeof(unsigned long long));
    unsigned long long* y = (unsigned long long*)alloc_cxl_memory(sizeof(unsigned long long));

    uint64_t cpu_violations = 0;

    auto start_time = std::chrono::high_resolution_clock::now();

#ifdef CUDA_GPU
    if (g_config.use_cuda) {
        // GPU-side results
        unsigned long long* dev_x = nullptr;
        unsigned long long* dev_y = nullptr;
        unsigned long long* dev_gpu_saw_x = nullptr;

        CUDA_CHECK(cudaMalloc(&dev_x, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&dev_y, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&dev_gpu_saw_x, g_config.iterations * sizeof(unsigned long long)));

        unsigned long long* gpu_saw_x = (unsigned long long*)malloc(g_config.iterations * sizeof(unsigned long long));

        // Run iterations
        for (uint64_t iter = 0; iter < g_config.iterations; iter++) {
            *x = 0;
            *y = 0;

            CUDA_CHECK(cudaMemcpy(dev_x, x, sizeof(unsigned long long), cudaMemcpyHostToDevice));
            CUDA_CHECK(cudaMemcpy(dev_y, y, sizeof(unsigned long long), cudaMemcpyHostToDevice));

            // Launch GPU (Thread B)
            CUDA_CHECK(launch_dekker_b(0, dev_x, dev_y, dev_gpu_saw_x, 1));

            // CPU Thread A: write x=1, fence, read y
            *x = 1;
            FLUSH(x);
            Fence();
            INVALIDATE(y);
            uint64_t cpu_saw_y = *y;

            // Wait for GPU
            CUDA_CHECK(cudaDeviceSynchronize());

            // Get GPU results
            CUDA_CHECK(cudaMemcpy(gpu_saw_x, dev_gpu_saw_x, sizeof(unsigned long long), cudaMemcpyDeviceToHost));

            // Check for SC violation
            if (cpu_saw_y == 0 && gpu_saw_x[0] == 0) {
                cpu_violations++;
            }
        }

        free(gpu_saw_x);
        CUDA_CHECK(cudaFree(dev_x));
        CUDA_CHECK(cudaFree(dev_y));
    } else
#endif
    {
        // CPU simulation: two threads acting as CPU and "GPU"
        for (uint64_t iter = 0; iter < g_config.iterations; iter++) {
            *x = 0;
            *y = 0;

            std::atomic_uint64_t gpu_saw_x(0);
            std::atomic_bool gpu_done(false);

            // "GPU" thread
            std::thread gpu_thread([&]() {
                *y = 1;
                FLUSH(y);
                Fence();
                INVALIDATE(x);
                uint64_t val = *x;
                gpu_saw_x.store(val, std::memory_order_release);
                gpu_done.store(true, std::memory_order_release);
            });

            // CPU thread
            *x = 1;
            FLUSH(x);
            Fence();
            INVALIDATE(y);
            uint64_t cpu_saw_y = *y;

            gpu_thread.join();

            // Check violation
            if (cpu_saw_y == 0 && gpu_saw_x.load(std::memory_order_acquire) == 0) {
                cpu_violations++;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    result.violations = cpu_violations;
    result.total_ops = g_config.iterations;
    result.pass = (cpu_violations == 0);  // Ideally no violations; non-zero indicates SC failure

    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    result.throughput_ops_per_sec = (result.total_ops * 1e6) / duration_us;

    free_cxl_memory(x, sizeof(unsigned long long));
    free_cxl_memory(y, sizeof(unsigned long long));

    return result;
}

// ============================================================================
// Test 3: Ticket Lock Contention Test
// ============================================================================

TestResults test_mutex_contention() {
    TestResults result = {"Mutex Contention", true, 0, 0, 0.0};

    // Allocate shared lock state and counter
    size_t lock_size = sizeof(std::atomic_uint64_t) * 2;
    unsigned long long* cxl_lock = (unsigned long long*)alloc_cxl_memory(lock_size);
    std::atomic_uint64_t* now_serving = (std::atomic_uint64_t*)&cxl_lock[0];
    std::atomic_uint64_t* next_ticket = (std::atomic_uint64_t*)&cxl_lock[8];

    unsigned long long* counter = (unsigned long long*)alloc_cxl_memory(sizeof(unsigned long long));
    *counter = 0;
    now_serving->store(0, std::memory_order_relaxed);
    next_ticket->store(0, std::memory_order_relaxed);

    auto start_time = std::chrono::high_resolution_clock::now();

#ifdef CUDA_GPU
    if (g_config.use_cuda) {
        unsigned long long* dev_ns = nullptr;
        unsigned long long* dev_nt = nullptr;
        unsigned long long* dev_counter = nullptr;

        CUDA_CHECK(cudaMalloc(&dev_ns, sizeof(std::atomic_uint64_t)));
        CUDA_CHECK(cudaMalloc(&dev_nt, sizeof(std::atomic_uint64_t)));
        CUDA_CHECK(cudaMalloc(&dev_counter, sizeof(unsigned long long)));

        CUDA_CHECK(cudaMemcpy(dev_ns, now_serving, sizeof(std::atomic_uint64_t), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(dev_nt, next_ticket, sizeof(std::atomic_uint64_t), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(dev_counter, counter, sizeof(unsigned long long), cudaMemcpyHostToDevice));

        // GPU threads
        CUDA_CHECK(launch_ticket_contention(0, dev_ns, dev_nt, dev_counter, g_config.gpu_threads, g_config.iterations));

        // CPU threads
        std::vector<std::thread> cpu_threads;
        for (int t = 0; t < g_config.cpu_threads; t++) {
            cpu_threads.emplace_back([&]() {
                for (uint64_t i = 0; i < g_config.iterations; i++) {
                    cpu_ticket_acquire(now_serving, next_ticket);
                    (*counter)++;  // Critical section
                    cpu_ticket_release(now_serving);
                }
            });
        }

        for (auto& t : cpu_threads) t.join();

        CUDA_CHECK(cudaDeviceSynchronize());

        // Copy results back
        CUDA_CHECK(cudaMemcpy(counter, dev_counter, sizeof(unsigned long long), cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(now_serving, dev_ns, sizeof(std::atomic_uint64_t), cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(next_ticket, dev_nt, sizeof(std::atomic_uint64_t), cudaMemcpyDeviceToHost));

        CUDA_CHECK(cudaFree(dev_ns));
        CUDA_CHECK(cudaFree(dev_nt));
        CUDA_CHECK(cudaFree(dev_counter));
    } else
#endif
    {
        // CPU simulation
        std::vector<std::thread> all_threads;

        // CPU threads
        for (int t = 0; t < g_config.cpu_threads; t++) {
            all_threads.emplace_back([&]() {
                for (uint64_t i = 0; i < g_config.iterations; i++) {
                    cpu_ticket_acquire(now_serving, next_ticket);
                    (*counter)++;
                    cpu_ticket_release(now_serving);
                }
            });
        }

        // "GPU" threads (simulated)
        for (int t = 0; t < g_config.gpu_threads; t++) {
            all_threads.emplace_back([&]() {
                for (uint64_t i = 0; i < g_config.iterations; i++) {
                    // Use CPU lock functions for simulation
                    cpu_ticket_acquire(now_serving, next_ticket);
                    (*counter)++;
                    cpu_ticket_release(now_serving);
                }
            });
        }

        for (auto& t : all_threads) t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    // Verify mutual exclusion: counter should equal total iterations
    uint64_t expected = (uint64_t)g_config.cpu_threads * g_config.iterations +
                       (uint64_t)g_config.gpu_threads * g_config.iterations;
    result.total_ops = expected;
    result.pass = (*counter == expected);
    result.violations = (expected > *counter) ? (expected - *counter) : 0;

    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    result.throughput_ops_per_sec = (result.total_ops * 1e6) / duration_us;

    free_cxl_memory(cxl_lock, lock_size);
    free_cxl_memory(counter, sizeof(unsigned long long));

    return result;
}

// ============================================================================
// Main Entry Point
// ============================================================================

void print_usage(const char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  --no-cuda              Force CPU simulation mode (ignore GPU)\n");
    printf("  --type2-info           Show detected Type-2 CXL devices and exit\n");
    printf("  --use-type2            Use Type-2 CXL device (cache0 by default)\n");
    printf("  --type2-device N       Select Type-2 device (0=cache0, 1=cache1)\n");
    printf("  --disable-type2-cache  Disable Type-2 DCOH cache (test UC semantics)\n");
    printf("  --test=order|...       Run specific test (order, dekker, mutex, all) (default: all)\n");
    printf("  --cpu-threads N        Number of CPU threads (default: 4)\n");
    printf("  --gpu-threads N        Number of GPU threads (default: 4)\n");
    printf("  --iterations K         Iterations per thread (default: 10000)\n");
    printf("  --csv                  CSV output format\n");
    printf("  --help, -h             Show this message\n");
}

int main(int argc, char* argv[]) {
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-cuda") == 0) {
            g_config.use_cuda = false;
        } else if (strcmp(argv[i], "--type2-info") == 0) {
            g_config.show_type2_info = true;
        } else if (strcmp(argv[i], "--use-type2") == 0) {
            g_config.use_type2 = true;
        } else if (strncmp(argv[i], "--type2-device=", 15) == 0) {
            g_config.use_type2 = true;
            g_config.type2_device_id = atoi(&argv[i][15]);
        } else if (strcmp(argv[i], "--disable-type2-cache") == 0) {
            if (g_config.type2_device_id >= 0 && g_config.type2_device_id < 2) {
                type2_sysfs_disable_cache(g_config.type2_device_id);
            }
        } else if (strncmp(argv[i], "--cpu-threads=", 14) == 0) {
            g_config.cpu_threads = atoi(&argv[i][14]);
        } else if (strncmp(argv[i], "--gpu-threads=", 14) == 0) {
            g_config.gpu_threads = atoi(&argv[i][14]);
        } else if (strncmp(argv[i], "--iterations=", 13) == 0) {
            g_config.iterations = atoll(&argv[i][13]);
        } else if (strcmp(argv[i], "--csv") == 0) {
            g_config.csv_output = true;
        } else if (strncmp(argv[i], "--test=", 7) == 0) {
            const char* test_name = &argv[i][7];
            g_config.test_order = g_config.test_dekker = g_config.test_mutex = false;
            if (strstr(test_name, "order")) g_config.test_order = true;
            if (strstr(test_name, "dekker")) g_config.test_dekker = true;
            if (strstr(test_name, "mutex")) g_config.test_mutex = true;
            if (strcmp(test_name, "all") == 0) {
                g_config.test_order = g_config.test_dekker = g_config.test_mutex = true;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Initialize Type-2 devices if needed
    if (g_config.show_type2_info || g_config.use_type2) {
        type2_sysfs_init();

        if (g_config.show_type2_info) {
            type2_sysfs_list_devices();
            return 0;
        }
    }

#ifdef CUDA_GPU
    // Check if GPU is available
    int device_count = 0;
    if (g_config.use_cuda) {
        if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
            fprintf(stderr, "Warning: No CUDA devices found. Switching to --no-cuda mode.\n");
            g_config.use_cuda = false;
        }
    }
#else
    g_config.use_cuda = false;
#endif

    // Initialize CXL system
    cxl_mutex_benchmark_init();

    std::vector<TestResults> results;

    if (!g_config.csv_output) {
        printf("=== CPU-GPU CXL Memory Access Test Suite ===\n");
        printf("Mode: %s\n", g_config.use_cuda ? "CUDA GPU" : "CPU Simulation");
        if (g_config.use_type2) {
            Type2DeviceInfo* dev = type2_sysfs_get_device(g_config.type2_device_id);
            if (dev && dev->cache_size > 0) {
                printf("Memory: Type-2 CXL cache%d (%.0f MiB, NUMA node %d, DCOH %s)\n",
                       g_config.type2_device_id,
                       dev->cache_size / (1024.0 * 1024.0),
                       dev->numa_node,
                       "enabled");
            }
        } else {
            printf("Memory: Generic CXL or DRAM\n");
        }
        printf("CPU threads: %d, GPU threads: %d, Iterations: %lu\n\n", g_config.cpu_threads, g_config.gpu_threads, g_config.iterations);
    }

    // Run tests
    if (g_config.test_order) {
        results.push_back(test_fetchadd_order());
    }
    if (g_config.test_dekker) {
        results.push_back(test_dekker_ordering());
    }
    if (g_config.test_mutex) {
        results.push_back(test_mutex_contention());
    }

    // Print results
    if (g_config.csv_output) {
        printf("test_name,pass,violations,total_ops,throughput_ops_per_sec\n");
        for (const auto& r : results) {
            printf("%s,%d,%lu,%lu,%.0f\n", r.test_name, r.pass, r.violations, r.total_ops, r.throughput_ops_per_sec);
        }
    } else {
        for (const auto& r : results) {
            printf("Test: %s\n", r.test_name);
            printf("  Pass: %s\n", r.pass ? "YES" : "NO");
            printf("  Violations: %lu\n", r.violations);
            printf("  Total Ops: %lu\n", r.total_ops);
            printf("  Throughput: %.0f ops/sec\n\n", r.throughput_ops_per_sec);
        }
    }

    cxl_mutex_benchmark_exit();
    return 0;
}
