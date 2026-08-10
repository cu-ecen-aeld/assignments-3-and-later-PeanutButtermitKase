#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{

    struct thread_data* thread_func_args =
        (struct thread_data *)thread_param;

    thread_func_args->thread_complete_success = false;

    if (usleep(thread_func_args->wait_to_obtain_ms * 1000) != 0) {
        ERROR_LOG("Failed while waiting %d ms before locking mutex",
                thread_func_args->wait_to_obtain_ms);
        return thread_param;
    }

    int lock_rc = pthread_mutex_lock(thread_func_args->mutex);
    if (lock_rc != 0) {
        ERROR_LOG("pthread_mutex_lock failed with error code %d", lock_rc);
        return thread_param;
    }

    if (usleep(thread_func_args->wait_to_release_ms * 1000) != 0) {
        ERROR_LOG("Failed while waiting %d ms before releasing mutex",
                thread_func_args->wait_to_release_ms);

        int unlock_rc = pthread_mutex_unlock(thread_func_args->mutex);
        if (unlock_rc != 0) {
            ERROR_LOG("pthread_mutex_unlock also failed during error recovery "
                    "with error code %d",
                    unlock_rc);
        }

        return thread_param;
    }

    int unlock_rc = pthread_mutex_unlock(thread_func_args->mutex);
    if (unlock_rc != 0) {
        ERROR_LOG("pthread_mutex_unlock failed with error code %d", unlock_rc);
        return thread_param;
    }

    thread_func_args->thread_complete_success = true;
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    struct thread_data *thread_data =
        malloc(sizeof(struct thread_data));

    if (thread_data == NULL) {
        ERROR_LOG("malloc failed");
        return false;
    }

    thread_data->mutex = mutex;
    thread_data->wait_to_obtain_ms = wait_to_obtain_ms;
    thread_data->wait_to_release_ms = wait_to_release_ms;
    thread_data->thread_complete_success = false;

    if (pthread_create(
            thread,
            NULL,
            threadfunc,
            thread_data) != 0) {

        ERROR_LOG("pthread_create failed");
        free(thread_data);
        return false;
    }

    return true;
}

