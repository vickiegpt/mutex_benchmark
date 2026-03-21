// Use -Dcxl to compile for CXL.


#ifndef __CXL_UTILS_HPP
#define __CXL_UTILS_HPP

#pragma once

#include <stddef.h>

// #ifndef __cpp_lib_hardware_interference_size
// namespace std {
//     const size_t hardware_destructive_interference_size = 64;
// }
// #endif


// defined via macro so they can be changed for the actual hardware
#ifdef cxl
    #include "stddef.h"
    extern "C" { 
        void *emucxl_alloc(size_t size, int node);
        void  emucxl_free(void *ptr, size_t size);
        void emucxl_init();
        void emucxl_exit();
    }
    #define ALLOCATE(size) emucxl_alloc(size, 1)
    #define FREE(ptr, size) emucxl_free(ptr, size)

    #define cxl_mutex_benchmark_init() emucxl_init()
    #define cxl_mutex_benchmark_exit() emucxl_exit()
#elif defined(hardware_cxl)
    #include <iostream>
    #include <thread>
    #include <chrono>
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <atomic>
    #include <cstring>
    #include <numaif.h>
    inline void *_cxl_region_init(size_t region_size) {
        void* mapped = mmap(nullptr, region_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) {
            perror("mmap");
            return nullptr;
        }

        unsigned long nodemask = 1UL; /* indicates physical memory node, might change */
        int mode = MPOL_BIND;
        unsigned long maxnode = sizeof(nodemask) * 8;
        // printf("mbind(%p, %ld, 0x%X, &0x%X, %ld, 0)\n", mapped, region_size, mode, nodemask, maxnode);
        if (mbind(mapped, region_size, mode, &nodemask, maxnode, 0) != 0) {
            perror("mbind");
        }

        return mapped;
    }

    inline void _cxl_region_free(void *region, size_t region_size) {
        munmap(region, region_size);
    }

    #define ALLOCATE(size) _cxl_region_init(size)
    #define FREE(ptr, size) _cxl_region_free(ptr, size)

    #define cxl_mutex_benchmark_init()
    #define cxl_mutex_benchmark_exit()
#elif defined(uc_cxl)
    // UC (uncacheable) access via device-backed mmap.
    // CXL memory exposed as a device file (e.g., DAX device or PCI BAR).
    // Pages are mapped UC by the kernel/driver, bypassing host cache entirely.
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <cstdio>

    #ifndef UC_DEVICE_PATH
        #define UC_DEVICE_PATH "/dev/dax0.0"
    #endif

    static int _uc_fd = -1;
    static size_t _uc_offset = 0;

    inline void *_uc_region_init(size_t region_size) {
        if (_uc_fd < 0) return nullptr;
        size_t page_size = sysconf(_SC_PAGESIZE);
        size_t aligned_size = (region_size + page_size - 1) & ~(page_size - 1);
        void *mapped = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, _uc_fd, _uc_offset);
        if (mapped == MAP_FAILED) {
            perror("mmap UC");
            return nullptr;
        }
        _uc_offset += aligned_size;
        return mapped;
    }

    inline void _uc_region_free(void *region, size_t region_size) {
        size_t page_size = sysconf(_SC_PAGESIZE);
        size_t aligned_size = (region_size + page_size - 1) & ~(page_size - 1);
        munmap(region, aligned_size);
    }

    inline void _uc_init() {
        _uc_fd = open(UC_DEVICE_PATH, O_RDWR);
        if (_uc_fd < 0) {
            perror("open UC device");
        }
        _uc_offset = 0;
    }

    inline void _uc_exit() {
        if (_uc_fd >= 0) {
            close(_uc_fd);
            _uc_fd = -1;
        }
    }

    #define ALLOCATE(size) _uc_region_init(size)
    #define FREE(ptr, size) _uc_region_free(ptr, size)
    #define cxl_mutex_benchmark_init() _uc_init()
    #define cxl_mutex_benchmark_exit() _uc_exit()
#else
    #define ALLOCATE(size) malloc(size)
    #define FREE(ptr, size) free(ptr); (void)(size) //

    #define cxl_mutex_benchmark_init()
    #define cxl_mutex_benchmark_exit()
#endif // CXL

#if defined(__x86_64)
    //#define Fence() __asm__ __volatile__ ( "mfence" )
    #define FENCE() __asm__ __volatile__ ( "lock; addq $0,128(%%rsp);" ::: "cc" )
#elif defined(__i386)
    #define FENCE() __asm__ __volatile__ ( "lock; addl $0,128(%%esp);" ::: "cc" )
#elif defined(__ARM_ARCH)
    #define FENCE() __asm__ __volatile__ ( "DMB ISH" ::: )
#else
    #error unsupported architecture
#endif

// Cache line flush macros for Cached-SC access mode.
// Under Cached-SC: clflushopt after stores ensures visibility to remote hosts;
//                  clflush + lfence before loads in polling loops ensures we see remote writes.
// Under UC: no-ops (UC bypasses cache entirely, no flush needed).
// Under default/Cached-HC: no-ops (hardware coherence manages visibility).
#if defined(cached_sc)
    #if defined(__x86_64) || defined(__i386)
        // Flush cache line containing addr to memory, then sfence for ordering.
        #define FLUSH(addr) do { \
            __asm__ __volatile__("clflushopt (%0)" :: "r"(addr) : "memory"); \
            __asm__ __volatile__("sfence" ::: "memory"); \
        } while(0)
        // Invalidate cache line containing addr so next load goes to memory.
        #define INVALIDATE(addr) do { \
            __asm__ __volatile__("clflush (%0)" :: "r"(addr) : "memory"); \
            __asm__ __volatile__("lfence" ::: "memory"); \
        } while(0)
    #else
        #error "Cached-SC flush not implemented for this architecture"
    #endif
#else
    #define FLUSH(addr) ((void)(addr))
    #define INVALIDATE(addr) ((void)(addr))
#endif

#endif // __CXL_UTILS_HPP