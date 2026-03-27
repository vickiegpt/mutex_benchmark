#ifndef EMUCXL_KERNEL_H
#define EMUCXL_KERNEL_H

#ifdef __linux__
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

typedef struct 
{ 
    int return_value;
} emucxl_arg_t;

typedef struct
{
    int size;
    int numa_node;
} emucxl_lib_t;

#ifdef __linux__
#define EMUCXL_INIT _IOW('e', 1, emucxl_arg_t *)
#define EMUCXL_EXIT _IO('e', 2)
#define EMUCXL_FREE _IO('e', 4)
#define EMUCXL_ALLOC _IOR('e', 3, emucxl_lib_t *)
#endif


#define FIRST_MINOR 0
#define MINOR_CNT 1
#define LOCAL_MEMORY 0
#define REMOTE_MEMORY 1

#endif