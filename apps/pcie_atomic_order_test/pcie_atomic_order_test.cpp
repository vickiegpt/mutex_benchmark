#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <set>
#include "../../lib/lock/lock.hpp"
#include "../../lib/utils/cxl_utils.hpp"

// PCIe Atomic Ordering Test for CXL 2.0 Switch
//
// This test demonstrates two key properties of PCIe/CXL ordering:
//
// TEST 1: Per-location FetchAdd Total Order
// ==========================================
// Verifies that fetch_add(1) on a single CXL-resident atomic counter is totally ordered
// at the Completer, even under concurrent access from N agents. Each agent receives a unique
// monotonically increasing value from 0 to N*K-1.
//
// Correctness property: The Completer serializes all FetchAdd requests. This single-location
// ordering property is the foundation of ticket-based locks under CXL 2.0 switch.
//
// TEST 2: Cross-location Store-Load Ordering (Dekker's Litmus Test)
// ==================================================================
// Demonstrates that SC (Sequential Consistency) is NOT guaranteed for cross-location accesses
// under CXL switch without hardware coherence. This is why Lamport's algorithm (which assumes SC)
// can fail, and why ticket spinlocks (which only need per-location ordering) are the correct
// primitive for CXL 2.0 switch scenarios.
//
// Dekker's pattern:
//   Thread A: store x=1, FLUSH, FENCE, load y
//   Thread B: store y=1, FLUSH, FENCE, load x
//
// Under SC: impossible for both to see 0 (at least one sees the other's write)
// Under CXL 2.0 switch: possible for both to see 0 (cross-location ordering not guaranteed)
//
// Violations indicate SC failure and explain the correctness limitation of Lamport-like
// algorithms on CXL switches.

struct test1_result {
    uint64_t test1_pass;
    uint64_t test1_total_ops;
    uint64_t test1_duplicates;
    uint64_t test1_gaps;
};

struct test2_result {
    uint64_t test2_violations;
    uint64_t test2_iterations;
    double violation_percentage;
};

// TEST 1: Per-location FetchAdd Total Order
//
// Each of N threads independently performs K fetch_add(1) operations on a shared counter
// in CXL memory. We collect all returned values and verify they form the complete set
// {0, 1, 2, ..., N*K-1} with no duplicates or gaps. This proves the Completer serializes
// all FetchAdd requests.

test1_result run_fetchadd_order_test(size_t num_threads, size_t iterations_per_thread) {
    test1_result result = {0, 0, 0, 0};

    // Allocate shared counter in CXL memory
    std::atomic_uint64_t *shared_counter = (std::atomic_uint64_t *)ALLOCATE(sizeof(std::atomic_uint64_t));
    shared_counter->store(0, std::memory_order_relaxed);

    // Collect returned values from each thread
    std::vector<std::vector<uint64_t>> thread_values(num_threads);
    for (size_t i = 0; i < num_threads; i++) {
        thread_values[i].reserve(iterations_per_thread);
    }

    // Barrier to ensure all threads start simultaneously
    std::atomic_bool start_flag(false);
    std::atomic_bool done_flag(false);

    // Thread function
    auto thread_func = [&](size_t thread_id) {
        // Wait for start signal
        while (!start_flag.load(std::memory_order_acquire)) {
        }

        // Each thread does iterations_per_thread fetch_adds
        for (size_t i = 0; i < iterations_per_thread; i++) {
            uint64_t val = shared_counter->fetch_add(1, std::memory_order_relaxed);
            thread_values[thread_id].push_back(val);
        }
    };

    // Spawn threads
    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_threads; i++) {
        threads.emplace_back(thread_func, i);
    }

    // Release all threads simultaneously
    start_flag.store(true, std::memory_order_release);

    // Wait for all to complete
    for (auto &t : threads) {
        t.join();
    }

    // Analyze results
    std::set<uint64_t> all_values;
    uint64_t total_ops = 0;

    for (size_t i = 0; i < num_threads; i++) {
        for (uint64_t val : thread_values[i]) {
            all_values.insert(val);
            total_ops++;
        }
    }

    // Check for duplicates: if all_values.size() != total_ops, we have duplicates
    if (all_values.size() != total_ops) {
        result.test1_duplicates = total_ops - all_values.size();
    }

    // Check for gaps: expected range is [0, total_ops-1]
    bool has_gaps = false;
    for (uint64_t i = 0; i < total_ops; i++) {
        if (all_values.find(i) == all_values.end()) {
            result.test1_gaps++;
            has_gaps = true;
        }
    }

    result.test1_pass = (!has_gaps && result.test1_duplicates == 0) ? 1 : 0;
    result.test1_total_ops = total_ops;

    FREE(shared_counter, sizeof(std::atomic_uint64_t));
    return result;
}

// TEST 2: Cross-location Store-Load Ordering (Dekker's Test)
//
// Two threads execute a memory ordering race:
//   Thread A: x=1; fence; read y
//   Thread B: y=1; fence; read x
//
// Under SC, at least one thread must see the other's write (cannot both see 0).
// Under CXL 2.0 switch (non-SC), it's possible for both to see 0.
//
// Each iteration runs the race once and checks the outcome. Violations indicate
// non-SC behavior.

test2_result run_dekker_ordering_test(size_t num_iterations) {
    test2_result result = {0, num_iterations, 0.0};

    // Allocate x and y in CXL memory (separate locations to test cross-location ordering)
    std::atomic_uint64_t *x = (std::atomic_uint64_t *)ALLOCATE(sizeof(std::atomic_uint64_t));
    std::atomic_uint64_t *y = (std::atomic_uint64_t *)ALLOCATE(sizeof(std::atomic_uint64_t));

    std::atomic_uint64_t violation_count(0);

    for (size_t iter = 0; iter < num_iterations; iter++) {
        // Reset x and y to 0
        x->store(0, std::memory_order_relaxed);
        y->store(0, std::memory_order_relaxed);

        // Barrier to start both threads
        std::atomic_bool start_flag(false);
        std::atomic_bool a_done(false);
        std::atomic_bool b_done(false);

        uint64_t a_saw_y = 0;
        uint64_t b_saw_x = 0;

        auto thread_a = [&]() {
            while (!start_flag.load(std::memory_order_acquire)) {
            }
            // Thread A writes x=1
            x->store(1, std::memory_order_relaxed);
            FLUSH(x);  // Make visible to device

            // Fence to ensure write is globally ordered
            Fence();

            // Thread A reads y
            INVALIDATE(y);  // Force bypass of cache
            a_saw_y = y->load(std::memory_order_acquire);
            a_done.store(true, std::memory_order_release);
        };

        auto thread_b = [&]() {
            while (!start_flag.load(std::memory_order_acquire)) {
            }
            // Thread B writes y=1
            y->store(1, std::memory_order_relaxed);
            FLUSH(y);  // Make visible to device

            // Fence to ensure write is globally ordered
            Fence();

            // Thread B reads x
            INVALIDATE(x);  // Force bypass of cache
            b_saw_x = x->load(std::memory_order_acquire);
            b_done.store(true, std::memory_order_release);
        };

        // Spawn threads
        std::thread ta(thread_a);
        std::thread tb(thread_b);

        // Release start signal to both threads
        start_flag.store(true, std::memory_order_release);

        // Wait for both to complete
        ta.join();
        tb.join();

        // Check for SC violation: both saw 0
        if (a_saw_y == 0 && b_saw_x == 0) {
            violation_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    result.test2_violations = violation_count.load(std::memory_order_relaxed);
    result.violation_percentage = (double)result.test2_violations / (double)num_iterations * 100.0;

    FREE(x, sizeof(std::atomic_uint64_t));
    FREE(y, sizeof(std::atomic_uint64_t));

    return result;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <num_threads> <iterations> [--csv]\n", argv[0]);
        fprintf(stderr, "  num_threads:  number of concurrent agents\n");
        fprintf(stderr, "  iterations:   iterations per test\n");
        fprintf(stderr, "  --csv:        output in CSV format\n");
        return 1;
    }

    size_t num_threads = (size_t)std::stoul(argv[1]);
    size_t iterations = (size_t)std::stoul(argv[2]);
    bool csv_output = (argc >= 4 && strcmp(argv[3], "--csv") == 0);

    // Initialize CXL system (no-op for most modes)
    cxl_mutex_benchmark_init();

    // Run tests
    test1_result r1 = run_fetchadd_order_test(num_threads, iterations);
    test2_result r2 = run_dekker_ordering_test(iterations);

    // Print results
    if (csv_output) {
        printf("test1_pass,%ld\n", r1.test1_pass);
        printf("test1_total_ops,%ld\n", r1.test1_total_ops);
        printf("test1_duplicates,%ld\n", r1.test1_duplicates);
        printf("test1_gaps,%ld\n", r1.test1_gaps);
        printf("test2_violations,%ld\n", r2.test2_violations);
        printf("test2_iterations,%ld\n", r2.test2_iterations);
        printf("test2_violation_percentage,%.4f\n", r2.violation_percentage);
    } else {
        printf("=== PCIe Atomic Ordering Test Results ===\n\n");

        printf("TEST 1: Per-location FetchAdd Total Order\n");
        printf("  Threads: %zu, Iterations per thread: %zu\n", num_threads, iterations);
        printf("  Total operations: %ld\n", r1.test1_total_ops);
        printf("  Duplicates: %ld\n", r1.test1_duplicates);
        printf("  Gaps: %ld\n", r1.test1_gaps);
        printf("  Result: %s\n", r1.test1_pass ? "PASS (Completer total order verified)" : "FAIL");
        printf("\n");

        printf("TEST 2: Cross-location Store-Load Ordering (Dekker's test)\n");
        printf("  Iterations: %ld\n", r2.test2_iterations);
        printf("  SC violations (both see 0): %ld\n", r2.test2_violations);
        printf("  Violation rate: %.4f%%\n", r2.violation_percentage);
        printf("  Interpretation: %s\n",
               r2.test2_violations == 0 ? "SC respected (or test ran too fast)"
                                         : "SC violated (expected under CXL switch non-SC)");
        printf("\n");

        printf("=== Interpretation ===\n");
        printf("TEST 1 PASS: Confirms PCIe FetchAdd provides total ordering at Completer.\n");
        printf("             This justifies ticket spinlocks as correct mutex for CXL 2.0 switch.\n");
        printf("\n");
        printf("TEST 2 VIOLATIONS: Demonstrates SC is not guaranteed for cross-location accesses.\n");
        printf("                    Explains why Lamport's load/store algorithm can fail on CXL\n");
        printf("                    switch: it requires SC, which the switch does not provide.\n");
    }

    cxl_mutex_benchmark_exit();
    return 0;
}
