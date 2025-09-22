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
    #include <numa.h>
    #include <numaif.h>
    inline void *_cxl_region_init(size_t region_size) {
        void* mapped = nullptr;

        // Get the current memory policy to respect numactl settings
        int mode;
        unsigned long nodemask[16] = {0}; // Support up to 1024 nodes
        unsigned long maxnode = sizeof(nodemask) * 8;

        // Try to get the current process memory policy
        if (get_mempolicy(&mode, nodemask, maxnode, nullptr, 0) == 0) {
            if (mode == MPOL_INTERLEAVE) {
                // For interleave mode, use numa_alloc_interleaved_subset
                struct bitmask *mask = numa_allocate_nodemask();
                for (int node = 0; node <= numa_max_node(); node++) {
                    if (nodemask[node / (sizeof(unsigned long) * 8)] & (1UL << (node % (sizeof(unsigned long) * 8)))) {
                        numa_bitmask_setbit(mask, node);
                    }
                }
                mapped = numa_alloc_interleaved_subset(region_size, mask);
                numa_free_nodemask(mask);
            } else if (mode == MPOL_BIND || mode == MPOL_PREFERRED) {
                // Find the first set node in the mask
                int preferred_node = 2; // Default to node 2 for CXL
                for (int node = 0; node <= numa_max_node(); node++) {
                    if (nodemask[node / (sizeof(unsigned long) * 8)] & (1UL << (node % (sizeof(unsigned long) * 8)))) {
                        preferred_node = node;
                        break;
                    }
                }
                mapped = numa_alloc_onnode(region_size, preferred_node);
            } else {
                // Default mode - use interleaved allocation
                mapped = numa_alloc_interleaved(region_size);
            }
        } else {
            // Fallback to interleaved allocation
            mapped = numa_alloc_interleaved(region_size);
        }

        if (mapped == NULL) {
            fprintf(stderr,"Problem allocating %zu bytes\n", region_size);
            perror("numa_alloc");
        }

        return mapped;
    }

    inline void _cxl_region_free(void *region, size_t region_size) {
        numa_free(region, region_size);
    }

    #define ALLOCATE(size) _cxl_region_init(size)
    #define FREE(ptr, size) _cxl_region_free(ptr, size)

    #define cxl_mutex_benchmark_init() 
    #define cxl_mutex_benchmark_exit()
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

#endif // __CXL_UTILS_HPP
