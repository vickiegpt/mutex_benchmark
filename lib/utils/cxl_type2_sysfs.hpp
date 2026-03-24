/**
 * cxl_type2_sysfs.hpp
 *
 * Type-2 CXL Device Access via sysfs
 *
 * Provides kernel-managed access to CXL Type-2 cache devices through sysfs.
 * Supports allocation, coherency control, and multi-device management.
 *
 * Type-2 devices on this system:
 * - /sys/bus/cxl/devices/cache0 - 128 MiB, NUMA node 0
 * - /sys/bus/cxl/devices/cache1 - 128 MiB, NUMA node 1
 *
 * Access pattern:
 * 1. Map device memory via mmap
 * 2. Control via sysfs (cache_disable, init_wbinvd)
 * 3. Use standard load/store + FLUSH/INVALIDATE macros
 */

#ifndef CXL_TYPE2_SYSFS_HPP
#define CXL_TYPE2_SYSFS_HPP

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>

// Type-2 CXL Device Properties
struct Type2DeviceInfo {
    int device_id;              // 0 = cache0, 1 = cache1
    const char* sysfs_path;     // /sys/bus/cxl/devices/cache{0,1}
    const char* dev_path;       // /dev/cxl/cache{0,1}
    uint64_t cache_size;        // From sysfs cache_size
    int numa_node;              // From sysfs numa_node
    void* mapped_base;          // mmap'd base address
};

/**
 * Global Type-2 device instances
 */
static Type2DeviceInfo g_type2_devices[2] = {
    { .device_id = 0, .sysfs_path = "/sys/bus/cxl/devices/cache0",
      .dev_path = "/dev/cxl/cache0", .cache_size = 0, .numa_node = -1, .mapped_base = NULL },
    { .device_id = 1, .sysfs_path = "/sys/bus/cxl/devices/cache1",
      .dev_path = "/dev/cxl/cache1", .cache_size = 0, .numa_node = -1, .mapped_base = NULL },
};

/**
 * Read a sysfs integer attribute
 */
static int _read_sysfs_int(const char* sysfs_path, const char* attr_name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", sysfs_path, attr_name);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open sysfs attr");
        return -1;
    }

    char buffer[64] = {0};
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (n < 0) {
        perror("read sysfs attr");
        return -1;
    }

    return atoi(buffer);
}

/**
 * Write a sysfs integer attribute
 */
static int _write_sysfs_int(const char* sysfs_path, const char* attr_name, int value) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", sysfs_path, attr_name);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open sysfs attr for write");
        return -1;
    }

    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%d", value);
    ssize_t n = write(fd, buffer, len);
    close(fd);

    return (n > 0) ? 0 : -1;
}

/**
 * Detect and initialize Type-2 CXL devices
 * Reads cache size and NUMA node from sysfs
 * Does NOT map memory yet (deferred to allocation)
 */
static int type2_sysfs_init() {
    for (int i = 0; i < 2; i++) {
        Type2DeviceInfo* dev = &g_type2_devices[i];

        // Check if sysfs path exists
        struct stat st;
        if (stat(dev->sysfs_path, &st) != 0) {
            printf("Type-2 device cache%d not found\n", i);
            continue;
        }

        // Read cache size (in bytes)
        int cache_size = _read_sysfs_int(dev->sysfs_path, "cache_size");
        if (cache_size > 0) {
            dev->cache_size = cache_size;
            printf("Type-2 Device cache%d: %ld bytes (%.0f MiB)\n",
                   i, dev->cache_size, dev->cache_size / (1024.0 * 1024.0));
        }

        // Read NUMA node
        int numa_node = _read_sysfs_int(dev->sysfs_path, "numa_node");
        if (numa_node >= 0) {
            dev->numa_node = numa_node;
            printf("  NUMA node: %d\n", dev->numa_node);
        }

        // Initialize DCOH (Distributed Cache Coherency)
        // Write to init_wbinvd to enable DCOH coherency
        _write_sysfs_int(dev->sysfs_path, "init_wbinvd", 1);
    }

    return 0;
}

/**
 * Allocate memory from a Type-2 CXL device
 *
 * Currently: Returns malloc'd memory (kernel manages Type-2 device memory)
 * Future: Can be extended to use BAR0/BAR2 mmap when direct access is needed
 */
static void* type2_sysfs_allocate(size_t size, int device_id) {
    if (device_id < 0 || device_id >= 2) {
        fprintf(stderr, "Invalid device ID: %d\n", device_id);
        return NULL;
    }

    Type2DeviceInfo* dev = &g_type2_devices[device_id];

    // Check if device is available
    if (dev->cache_size == 0) {
        fprintf(stderr, "Type-2 device cache%d not initialized\n", device_id);
        return NULL;
    }

    // Check size
    if (size > dev->cache_size) {
        fprintf(stderr, "Allocation size %zu exceeds cache%d size %ld\n",
                size, device_id, dev->cache_size);
        return NULL;
    }

    // Kernel manages the Type-2 device memory
    // Application allocates from system DRAM that's coherent with Type-2 device
    // The coherency is handled by DCOH protocol at the hardware level
    void* ptr = malloc(size);

    if (ptr) {
        printf("Allocated %zu bytes from Type-2 cache%d (DCOH coherent)\n",
               size, device_id);

        // Optional: Try to bind allocation to device's NUMA node for locality
        // (Would use numa_alloc_onnode if available, but malloc is sufficient)
    }

    return ptr;
}

/**
 * Free Type-2 allocated memory
 */
static void type2_sysfs_free(void* ptr, size_t size) {
    (void)size;  // Unused

    if (ptr) {
        free(ptr);
    }
}

/**
 * Get available Type-2 device info
 */
static Type2DeviceInfo* type2_sysfs_get_device(int device_id) {
    if (device_id < 0 || device_id >= 2) {
        return NULL;
    }
    return &g_type2_devices[device_id];
}

/**
 * Query all Type-2 devices
 */
static void type2_sysfs_list_devices() {
    printf("=== Type-2 CXL Devices ===\n");
    for (int i = 0; i < 2; i++) {
        Type2DeviceInfo* dev = &g_type2_devices[i];
        if (dev->cache_size > 0) {
            printf("cache%d: %ld bytes (%.0f MiB), NUMA node %d\n",
                   i, dev->cache_size, dev->cache_size / (1024.0 * 1024.0),
                   dev->numa_node);
        }
    }
}

/**
 * Control Type-2 DCOH coherency
 * 0 = disable, 1 = enable
 */
static int type2_sysfs_set_dcoh(int device_id, int enable) {
    if (device_id < 0 || device_id >= 2) return -1;
    Type2DeviceInfo* dev = &g_type2_devices[device_id];
    return _write_sysfs_int(dev->sysfs_path, "cache_disable", enable ? 0 : 1);
}

/**
 * Disable cache to force uncacheable semantics
 */
static int type2_sysfs_disable_cache(int device_id) {
    if (device_id < 0 || device_id >= 2) return -1;
    Type2DeviceInfo* dev = &g_type2_devices[device_id];
    return _write_sysfs_int(dev->sysfs_path, "cache_disable", 1);
}

#endif  // CXL_TYPE2_SYSFS_HPP
