#ifndef LAMPORT_LOCK_HPP
#define LAMPORT_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "trylock.hpp"
#include "../utils/cxl_utils.hpp"
#include <stdexcept>
#include <iostream>


class LamportLock : public virtual TryLock {
public:
    // struct ThreadData {

    // };

    static size_t get_cxl_region_size(size_t num_threads) {
        return sizeof(size_t) * 2 + std::hardware_destructive_interference_size * (num_threads + 1) + sizeof(bool) + 7; // padding
    }

    void init(size_t num_threads) override {
        volatile char *_cxl_region = (volatile char*)ALLOCATE(get_cxl_region_size(num_threads));
        region_init(num_threads, _cxl_region);
    }

    inline volatile bool *get_b(size_t thread_id) {
        volatile bool *result = (volatile bool*)&this->_cxl_region[thread_id * std::hardware_destructive_interference_size];
        return result;
    }

    void region_init(size_t num_threads, volatile char *_cxl_region) override {
        this->_cxl_region = _cxl_region;
        size_t offset = 0;
        offset += std::hardware_destructive_interference_size * (num_threads + 1);
        this->x = (volatile size_t*)&_cxl_region[offset];
        offset += sizeof(size_t);
        this->y = (volatile size_t*)&_cxl_region[offset];
        offset += sizeof(size_t);
        this->fast = (volatile bool*)&_cxl_region[offset];
        *fast = false;
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
    start:
        volatile bool *my_b = get_b(thread_id);
        *my_b = true; //trying to grab the lock
        FLUSH(my_b);
        *x = thread_id+1; //first confirmation
        FLUSH(x);
        Fence();

        INVALIDATE(y);
        if (*y!=0){
            *my_b = false; //no longer going for the lock
            FLUSH(my_b);
            while (*y!=0){ INVALIDATE(y); } //wait for whoever was trying to get it to get it
            goto start; //restart
        }

        *y = thread_id + 1; //second confirmation
        FLUSH(y);
        Fence();

        INVALIDATE(x);
        if (*x!=thread_id+1){ //someone started going for the lock
            *my_b = false; //not longer going for the lock
            FLUSH(my_b);

            for (int j=0; j<(int)num_threads; j++){while(*get_b(j)){ INVALIDATE(get_b(j)); }} //wait for contention to go down
            Fence();

            INVALIDATE(y);
            if (*y!=thread_id+1){ //while waiting, someone messed with second confirmation
                while(*y!=0){ INVALIDATE(y); } //wait for the person to unlock
                goto start;
            }
        }
    }
    
    bool trylock(size_t thread_id){
        volatile bool *my_b = get_b(thread_id);
        *x = thread_id+1; //first confirmation
        FLUSH(x);
        Fence();

        INVALIDATE(y);
        if (*y!=0){
            *my_b = false; //no longer going for the lock
            FLUSH(my_b);
            return false; //wait for whoever was trying to get it to get it
        }

        *y = thread_id + 1; //second confirmation
        FLUSH(y);
        Fence();

        INVALIDATE(x);
        if (*x!=thread_id+1){ //someone started going for the lock
            *my_b = false; //not longer going for the lock
            FLUSH(my_b);
            Fence();
            for (int j=0; j<(int)num_threads; j++){while(*get_b(j)){ INVALIDATE(get_b(j)); }} //wait for contention to go down


            INVALIDATE(y);
            if (*y!=thread_id+1){ //while waiting, someone messed with second confirmation
                return false; //wait for the person to unlock
            }
        }
        bool leader = false;
        INVALIDATE(fast);
        if (!*fast){
            leader=true;
            *fast=leader;
            FLUSH(fast);
        }
        *y=0;
        FLUSH(y);
        *my_b=false;
        FLUSH(my_b);
        return leader;
    }

    void unlock(size_t thread_id) override {
        *y=0;
        FLUSH(y);
        Fence();
        *get_b(thread_id) = false;
        FLUSH(get_b(thread_id));
        *fast=false;
        FLUSH(fast);
    }

    void destroy() override {
        FREE((void*)_cxl_region, get_cxl_region_size(num_threads));
    }

    std::string name() override {
        return "lamport";
    }
private:
    volatile char *_cxl_region;
    volatile size_t *x;
    volatile size_t *y;
    volatile bool *fast;

    size_t num_threads;
};

#endif // LAMPORT_LOCK_HPP