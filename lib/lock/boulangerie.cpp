#include "../utils/cxl_utils.hpp"

#include "lock.hpp"
#include <stdexcept>
#include <cstring>
#include <new>

class Boulangerie : public virtual SoftwareMutex {
public:
    struct ThreadData {
        volatile int number;
        volatile bool choosing;
    };

    void init(size_t num_threads) override {
        _cxl_region_size = std::hardware_destructive_interference_size * num_threads;
        this->_cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);
        this->num_threads = num_threads;
        memset((void*)this->_cxl_region, 0, _cxl_region_size);
    }

    inline ThreadData *get(size_t thread_id) {
        return (ThreadData*)&this->_cxl_region[thread_id * std::hardware_destructive_interference_size];
    }

    void lock(size_t thread_id) override {
        ThreadData *td = get(thread_id);

        td->choosing = 1;
        FLUSH(&td->choosing);
        FENCE();
        int max_number = 0;
        ThreadData *other_thread;
        for (size_t i = 0; i < num_threads; ++i) {
            other_thread = get(i);
            INVALIDATE(&other_thread->number);
            if (other_thread->number > max_number) {
                max_number = other_thread->number;
            }
        }
        td->number = max_number + 1;
        FLUSH(&td->number);
        FENCE();
        td->choosing = 0;
        FLUSH(&td->choosing);
        FENCE();

        //limit the number of thread to check
        size_t limit;
        if (td->number == 1 && thread_id > 0) {
            limit = thread_id;
        }
        else {
            limit = num_threads;
        }

        int prev_j, curr_j;
        for (size_t j = 0; j < limit; ++j) {
            other_thread = get(j);
            if (j == thread_id) {
                continue;
            }
            while (other_thread->choosing) {
                INVALIDATE(&other_thread->choosing);
                // Wait for that thread to be done choosing a number.
            }
            prev_j = other_thread->number;
            curr_j = other_thread->number;
            while (curr_j != 0 && (curr_j < td->number || (curr_j == td->number && j < thread_id)) && curr_j == prev_j) {
                prev_j = curr_j;
                INVALIDATE(&other_thread->number);
                curr_j = other_thread->number;
            }
        }
        FENCE();
    }

    void unlock(size_t thread_id) override {
        get(thread_id)->number = 0;
        FLUSH(&get(thread_id)->number);
    }

    void destroy() override {
        FREE((void*)this->_cxl_region, _cxl_region_size);
    }
    
    std::string name() override { 
        return "boulangerie"; 
    }

private:
    volatile char *_cxl_region;
    size_t num_threads;
    size_t _cxl_region_size;
};


