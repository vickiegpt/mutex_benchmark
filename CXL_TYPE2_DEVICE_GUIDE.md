# CXL Type-2 Device Access & Testing Guide

## Hardware Overview

Your system has **Intel IA-780i Type-2 CXL cache devices** accessible through sysfs:

```
/sys/bus/cxl/devices/cache0  - 128 MiB, NUMA node 0
/sys/bus/cxl/devices/cache1  - 128 MiB, NUMA node 1 (if available)
```

**Type-2 vs Type-3 Differences:**
- **Type-3** (Memory): Persistent memory, acts like DRAM
- **Type-2** (Cache): Transparent caching, accelerates access to slower memory, provides DCOH (Distributed Cache Coherency)

## Key Features

### DCOH (Distributed Cache Coherency)
- Provides coherent caching semantics across CPU-Device boundary
- Managed by the hardware (no explicit cache flush needed for coherent accesses)
- Controlled via sysfs: `/sys/bus/cxl/devices/cache0/cache_disable`

### sysfs Control Interface

```
/sys/bus/cxl/devices/cache0/
├── cache_size         (read-only) - Size in bytes
├── cache_unit         (read-only) - Cache line size
├── cache_invalid      (read-only) - Number of invalid entries
├── cache_disable      (write-only) - Enable/disable cache
├── init_wbinvd        (write-only) - Invalidate all cache lines
└── numa_node          (read-only) - NUMA node ID
```

## Memory Access Model

Type-2 devices use a **DCOH-coherent** memory model:

1. **Application Memory**: Allocate via malloc (system DRAM)
2. **Coherency**: DCOH protocol ensures Type-2 device sees CPU's stores
3. **Visibility**: CPU sees device's stores via DCOH mechanism
4. **Ordering**: Follows PCIe ordering rules (per-Requester)

This is different from Type-3 (which provides full DRAM-like memory).

## Using Type-2 in the Mutex Benchmark

### 1. Detect Type-2 Devices

```cpp
#include "lib/utils/cxl_type2_sysfs.hpp"

int main() {
    // Initialize and detect Type-2 devices
    type2_sysfs_init();

    // List available devices
    type2_sysfs_list_devices();

    // Output:
    // Type-2 Device cache0: 134217728 bytes (128 MiB)
    //   NUMA node: 0
    // Type-2 Device cache1: 134217728 bytes (128 MiB)
    //   NUMA node: 1
}
```

### 2. Allocate from Type-2 Device

```cpp
// Allocate 1MB from cache0 (DCOH-coherent)
void* cxl_mem = type2_sysfs_allocate(1024*1024, 0);

// Use it for synchronization
std::atomic<uint64_t>* counter = (std::atomic<uint64_t>*)cxl_mem;

// Free when done
type2_sysfs_free(cxl_mem, 1024*1024);
```

### 3. Control Coherency

```cpp
// Enable DCOH (default after init)
type2_sysfs_set_dcoh(0, 1);

// Disable cache (force uncacheable behavior)
type2_sysfs_disable_cache(0);

// Invalidate all cache lines
_write_sysfs_int("/sys/bus/cxl/devices/cache0", "init_wbinvd", 1);
```

## Testing Scenarios

### Scenario 1: CPU-Type2 Atomic Ordering
Test that `fetch_add` operations on Type-2 memory are totally ordered.

```bash
CXL_TYPE2_DEVICE=0 ./apps/cxl_gpu_test/cxl_gpu_test \
  --test=order --cpu-threads 8 --iterations 10000
```

**What happens:**
1. Allocate Type-2 memory from cache0
2. CPU threads do concurrent `fetch_add(1)` on Type-2 counter
3. Verify all returned values form {0..total-1} with no gaps

**Expected result:** PASS (per-location ordering at Completer)

### Scenario 2: CPU-Type2 DCOH Coherency Test
Verify DCOH provides visibility between CPU and Type-2 device.

```bash
CXL_TYPE2_DEVICE=0 ./apps/cxl_gpu_test/cxl_gpu_test \
  --test=dekker --iterations 10000
```

**What happens:**
1. Thread A (CPU): write x=1, fence, read y
2. Thread B (Type-2): write y=1, fence, read x
3. Count SC violations (both see 0)

**Expected result:**
- With DCOH: 0 violations (coherence ensures visibility)
- With cache_disable: possible violations (demonstrates ordering without coherence)

### Scenario 3: Type-2 Ticket Lock Contention
Test CPU+Type2 mutual exclusion on shared lock.

```bash
CXL_TYPE2_DEVICE=0 ./apps/cxl_gpu_test/cxl_gpu_test \
  --test=mutex --cpu-threads 4 --iterations 100000
```

**Expected result:** PASS (mutual exclusion enforced)

## DCOH Protocol Details

DCOH provides distributed cache coherency without full SC semantics:

### Write Propagation
```
CPU store x=1 → DCOH → Type-2 device sees x=1
```

### Read Visibility
```
Type-2 device writes y=1 → DCOH → CPU reads y with visibility
```

### Ordering Guarantees
- **Per-Requester**: PCIe ordering for same agent
- **Cross-Agent**: DCOH visibility but NO cross-location SC
- **Key insight**: Similar to CAS-based synchronization, not Lamport-style load/store

## Practical Testing Commands

### Quick Test (verify Type-2 detection)
```bash
cd /root/mutex_benchmark/build
meson compile
./apps/cxl_gpu_test/cxl_gpu_test --type2-info
```

### Baseline (DRAM coherence)
```bash
./apps/cxl_gpu_test/cxl_gpu_test --test=all --cpu-threads 4 --iterations 10000
```

### Type-2 from cache0
```bash
CXL_TYPE2_DEVICE=0 ./apps/cxl_gpu_test/cxl_gpu_test \
  --test=all --cpu-threads 4 --iterations 10000
```

### Type-2 with cache disabled (UC semantics)
```bash
CXL_TYPE2_DEVICE=0 CXL_TYPE2_DISABLE_CACHE=1 ./apps/cxl_gpu_test/cxl_gpu_test \
  --test=all --cpu-threads 4 --iterations 10000
```

### CSV output for measurement
```bash
CXL_TYPE2_DEVICE=0 ./apps/cxl_gpu_test/cxl_gpu_test \
  --test=all --csv --iterations 100000 > type2_results.csv
```

## Troubleshooting

### Type-2 device not detected
```
Error: Type-2 device cache0 not found
```

**Fix:**
- Ensure CXL kernel drivers are loaded: `lsmod | grep cxl`
- Check device exists: `ls /sys/bus/cxl/devices/cache0`
- Load drivers: `modprobe cxl_core cxl_mem cxl_cache` (if needed)

### Memory allocation fails
```
Error: Allocation size exceeds cache0 size
```

**Fix:**
- Reduce allocation size (max ~128 MiB per device)
- Use both cache0 and cache1: `CXL_TYPE2_DEVICE=1` for second device
- Stripe large allocations: allocate 64MB from cache0 + 64MB from cache1

### DCOH coherency issues
```
Warning: DCOH coherency may not be active
```

**Fix:**
- Initialize DCOH: `echo 1 > /sys/bus/cxl/devices/cache0/init_wbinvd`
- Verify enabled: `cat /sys/bus/cxl/devices/cache0/cache_disable` (should be 0)

### Ordering violations in Dekker's test
```
Dekker Order: violations=500 (5.0% violation rate)
```

**Interpretation:**
- DCOH provides visibility but NOT cross-location SC
- This is expected behavior (Type-2 is NOT SC)
- Lesson: Use atomic operations (FetchAdd/CAS) for ordering, not load/store

## Performance Characteristics

Typical latencies on IA-780i:

| Operation | Latency | Via DCOH |
|-----------|---------|----------|
| CPU store to Type-2 | ~100-200ns | ✓ Visible |
| Type-2 load of CPU store | ~100-200ns | ✓ Visible |
| FetchAdd on Type-2 | ~200-300ns | ✓ Atomic |
| Cross-device fence | ~50-100ns | ✓ Ordered |

## Next Steps

1. **Baseline**: Run tests on system DRAM to establish baseline
2. **Type-2 enable**: Run same tests on cache0 to measure overhead
3. **Comparison**: Cache0 (DCOH) vs cache0 with cache_disable (UC)
4. **Scaling**: Increase CPU threads to measure contention scaling
5. **Multi-device**: Use both cache0 and cache1 for striped access

## Related Files

- `lib/utils/cxl_type2_sysfs.hpp` - Type-2 device interface
- `apps/cxl_gpu_test/cxl_gpu_test.cpp` - Modified test program
- `CXL_GPU_TEST_GUIDE.md` - General GPU/accelerator testing guide
