
#ifdef hardware_cxl
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
void *_cxl_region_init(size_t region_size) {
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

void _cxl_region_free(void *region, size_t region_size) {
    numa_free(region, region_size);
    // munmap(region, region_size);
}
#endif
