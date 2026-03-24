# CPU-GPU CXL Memory Access Testing Guide

This guide explains the CPU-GPU CXL test suite implementation, how to use it, and what to expect from the results.

## Overview

The `cxl_gpu_test` application tests the correctness and performance of mutual exclusion and memory ordering when both CPU and GPU threads access shared CXL memory on a single host. This models a "two-device" CXL scenario where:
- **Device A**: CPU as PCIe Requester (using LOCK XADD for atomic operations)
- **Device B**: GPU as PCIe Requester (using CUDA atomicAdd for atomic operations)
- **Shared memory**: CXL device memory as the Completer

## Hardware Setup

### Current System State
- **CPU**: Intel Xeon 6787P (Granite Rapids) with PCIe 5.0 / CXL 2.0 native support
- **CXL Memory**: Decoder at 0x4080000000, ~126 GB available (mem0, mem1 devices)
- **GPU**: NVIDIA GPU present but **NVIDIA driver NOT loaded**
- **CUDA**: CUDA 12.0 installed (`/usr/bin/nvcc`), toolchain available
- **NUMA**: Only 2 nodes visible (0, 1) — CXL memory not yet configured as separate NUMA node

### To Enable GPU Testing (Prerequisites)

Before running GPU tests, you must:

```bash
# 1. Load NVIDIA kernel driver
sudo modprobe nvidia

# 2. Verify GPU is accessible
nvidia-smi

# 3. Rebuild the test suite (meson will auto-detect CUDA)
cd /root/mutex_benchmark/build
meson compile
```

If the driver fails to load, ensure:
- The system has an NVIDIA GPU physically present
- The NVIDIA driver is compatible with your kernel (check `dmesg` for errors)
- SELinux or AppArmor isn't blocking the driver (if applicable)

## Build & Test

### Quick Start (CPU Simulation - Works Today)

```bash
cd /root/mutex_benchmark/build
meson compile

# Run all tests in CPU simulation mode (no GPU needed)
CXL_NUMA_NODE=0 ./apps/cxl_gpu_test/cxl_gpu_test --no-cuda --test=all --cpu-threads 4 --gpu-threads 4 --iterations 10000
```

### With GPU (After Driver Load)

```bash
# Load driver
sudo modprobe nvidia

# Rebuild (auto-detects CUDA)
cd /root/mutex_benchmark/build
meson compile

# Run with GPU
./apps/cxl_gpu_test/cxl_gpu_test --test=all --cpu-threads 4 --gpu-threads 32 --iterations 10000
```

### CSV Output (for Data Collection)

```bash
./apps/cxl_gpu_test/cxl_gpu_test --no-cuda --test=all --csv --iterations 100000 > results.csv
```

## Test Descriptions

### Test 1: FetchAdd Total Order Test

**Purpose**: Verify that `fetch_add` (CPU) and `atomicAdd` (GPU) operations on a single shared counter are totally ordered at the CXL Completer.

**Setup**:
- Allocate one shared atomic counter in CXL memory
- N_CPU CPU threads + N_GPU GPU threads each do K iterations of `fetch_add(counter, 1)`
- Each operation records the returned value (pre-increment value)
- Total: N_CPU * K + N_GPU * K unique values returned

**Expected Result**:
- All returned values form a complete set {0, 1, 2, ..., total_ops-1}
- No duplicates, no gaps
- **PASS**: Confirms per-location atomicity at Completer
- **FAIL**: Indicates ordering corruption

**Interpretation**:
- ✅ PASS in both CPU and GPU modes: FetchAdd provides Completer serialization
- This justifies ticket spinlocks as correct primitives for CXL 2.0 switch

**Throughput**: ~6M - 22M ops/sec (CPU simulation), higher with GPU

---

### Test 2: Dekker's Cross-Location Ordering Test

**Purpose**: Demonstrate why Sequential Consistency (SC) is NOT guaranteed across multiple locations under CXL switch topology. This explains why Lamport's load/store algorithm fails on non-SC systems.

**Setup** (classic Dekker's test):
```
CPU Thread A:        GPU Thread B:
  store x = 1          store y = 1
  FLUSH + FENCE        THREADFENCE_SYSTEM
  load y               load x
  → recorded as a_saw_y → recorded as b_saw_x
```

- Run 1000+ iterations of this race
- Count occurrences of "both see 0" (SC violation)

**Expected Result**:
- **CPU mode**: 0 violations (hardware cache coherence maintains SC)
- **GPU mode with CXL**: Possible non-zero violations (demonstrates non-SC)
- **PASS**: 0 violations (SC respected, or test ran too fast)
- **FAIL**: > 0 violations (SC violated — expected under non-coherent CXL)

**Interpretation**:
- ✅ 0 violations: SC is maintained (or the window is tight)
- ⚠️ > 0 violations: SC is NOT maintained (expected for cross-location access on CXL switch)
- This explains why Lamport's algorithm (which requires SC) doesn't work on CXL without modifications

**Throughput**: ~4K - 16K ops/sec (expensive synchronization)

---

### Test 3: Ticket Lock Contention Test

**Purpose**: Verify that a ticket lock using atomic FetchAdd is correct under concurrent CPU-GPU contention on shared CXL memory.

**Setup**:
- Allocate shared lock state (now_serving, next_ticket counters)
- Allocate shared counter in critical section
- N_CPU CPU threads + N_GPU GPU threads contend on the same ticket lock
- Each thread acquires lock K times, increments counter, releases lock

**Expected Result**:
- Final counter value = (N_CPU + N_GPU) * K (one increment per critical section entry)
- **PASS**: Counter equals expected value (mutual exclusion enforced)
- **FAIL**: Counter < expected value (lost increments indicate non-mutual exclusion)

**Interpretation**:
- ✅ PASS: Ticket lock enforces mutual exclusion with CPU+GPU
- ❌ FAIL: Indicates atomicity or visibility bug in lock implementation

**Throughput**: ~420K - 587K ops/sec (CPU simulation), higher with GPU

---

## Implementation Details

### Memory Access Modes

The test uses the **hardware_cxl** mode which:
1. Allocates memory via `mmap(MAP_SHARED | MAP_ANONYMOUS)`
2. Binds to CXL NUMA node via `mbind(MPOL_BIND, node)`
3. Defaults to node 2, but can be overridden: `CXL_NUMA_NODE=0 ./app`

On this system, use `CXL_NUMA_NODE=0` since there are only 2 NUMA nodes.

### CPU-Side Atomics

CPU threads use:
- `std::atomic<uint64_t>::fetch_add(1, relaxed)` → **LOCK XADD** instruction → **PCIe FetchAdd TLP** to CXL Completer
- `INVALIDATE(ptr)` before loads → `clflush + lfence` in cached_sc mode → forces read from CXL device memory
- `FLUSH(ptr)` after stores → `clflushopt + sfence` in cached_sc mode → writes back to CXL device memory

### GPU-Side Atomics

GPU threads use (requires Volta SM 7.0+):
- `atomicAdd_system(ptr, 1)` → **PCIe AtomicOp TLP** to CXL Completer (SM 7.0+)
- `__threadfence_system()` → system-wide fence that crosses PCIe boundary
- Regular `atomicAdd(ptr, 1)` → GPU-scoped atomic (not guaranteed PCIe-atomic) on older SM

**Difference**:
- `atomicAdd()` is only atomic at GPU's L2 cache, NOT a PCIe atomic from other agents' perspective
- `atomicAdd_system()` generates a true PCIe AtomicOp that is serialized at the CXL Completer

For correctness, SM 7.0+ (`-arch=sm_70` in nvcc) is required. The meson.build specifies this.

### Synchronization Across Devices

Key insight: **CPU and GPU communicate only through CXL device memory**.

- CPU writes to CXL: `store + FLUSH` puts the value in device memory
- GPU writes to CXL: `atomicAdd_system + __threadfence_system` puts the value in device memory
- GPU reads CPU's write: `__threadfence_system` then `load` reads from device memory
- CPU reads GPU's write: `INVALIDATE + load` forces a read from device memory

No shared cache, no coherence protocol — only explicit cache management and fences.

### Build System

The `meson.build` for cxl_gpu_test:
- Auto-detects `nvcc` and CUDA 12.0+
- If CUDA found: compiles `.cu` kernels with `-arch=sm_70`
- If CUDA not found: builds CPU-only mode (test framework still works, GPU code is #ifdef'd out)
- Linking: `-lcudart` and `/usr/local/cuda/lib64` paths

## Expected Results

### CPU Simulation Mode (--no-cuda)
```
Test: FetchAdd Order
  Pass: YES           # All values 0..79999 with no gaps
  Violations: 0
  Total Ops: 80000
  Throughput: 6219389 ops/sec

Test: Dekker Order
  Pass: YES           # 0 violations (DRAM has coherence)
  Violations: 0
  Total Ops: 10000
  Throughput: 4109 ops/sec

Test: Mutex Contention
  Pass: YES           # Counter == 80000 (mutual exclusion holds)
  Violations: 0
  Total Ops: 80000
  Throughput: 421839 ops/sec
```

### GPU Mode (After NVIDIA Driver Load)
Expected similar results, but:
- **FetchAdd Order**: Should still PASS (per-location ordering at Completer)
- **Dekker Order**: May show violations > 0 (demonstrates cross-location non-SC under CXL switch)
- **Mutex Contention**: Should PASS (ticket lock still correct)
- **Throughput**: Higher (GPU can do more ops in parallel)

## Key Insights

1. **PCIe provides per-location total ordering**: FetchAdd operations on a single address are totally ordered at the Completer. This is why Test 1 PASSES.

2. **PCIe does NOT provide cross-location SC**: Writes to different addresses from different Requesters can be seen in different orders. This is why Test 2 MAY show violations (especially on real CXL hardware).

3. **Ticket spinlocks are correct for CXL**: Because they only need per-location ordering (all contention serialized on `next_ticket`), not SC.

4. **Lamport's algorithm is NOT correct for CXL**: Because it depends on SC to detect write conflicts across multiple shared variables (`x`, `y`, `b[]`).

## CSV Output Format

```csv
test_name,pass,violations,total_ops,throughput_ops_per_sec
FetchAdd Order,1,0,80000,22340128
Dekker Order,1,0,10000,16315
Mutex Contention,1,0,80000,587790
```

Fields:
- `test_name`: Name of the test
- `pass`: 1 = PASS, 0 = FAIL
- `violations`: Number of failures/violations detected
- `total_ops`: Total atomic operations performed
- `throughput_ops_per_sec`: Aggregate throughput

## Troubleshooting

### "CUDA devices not found" warning
- NVIDIA driver not loaded: run `sudo modprobe nvidia`
- No GPU present: hardware issue
- App falls back to --no-cuda automatically

### NUMA allocation errors
- "mbind: Invalid argument" → NUMA node doesn't exist
- Fix: Set correct node with `CXL_NUMA_NODE=0` (or other valid node)
- Check valid nodes: `numactl --hardware`

### Dekker test shows unexpected violations
- In --no-cuda mode: should be 0 (coherent DRAM)
- In GPU mode: non-zero expected (demonstrates the non-SC issue)
- Very high violation rate (>50%): indicates broken synchronization

## Next Steps

1. **Load NVIDIA driver** and test GPU mode to observe cross-location ordering effects
2. **Configure CXL as NUMA node** for proper CXL-only testing (requires `cxl create-region`, `ndctl`)
3. **Collect performance data** for paper via CSV output
4. **Extend with custom workloads** in the critical section (currently just `counter++`)
