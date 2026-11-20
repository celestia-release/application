#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include "hal_util_mutex.h"

//pthread_mutex_t


/*****************************************************
 * mutex create function
 * param: none.
 * return: util_mutex_t * --- struct address.
 ****************************************************/
util_mutex_t * util_mutex_create(void)
{
    util_mutex_t *mutex = (util_mutex_t *)malloc(sizeof(util_mutex_t));
    if (!mutex)
    {
        return NULL;
    }
    pthread_mutex_t *handle_th = malloc(sizeof(pthread_mutex_t));
    mutex->mutex_handle = (void *)handle_th;
    // POSIX平台使用互斥锁
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);

    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); // 支持递归锁

    int ret = pthread_mutex_init((pthread_mutex_t*)mutex->mutex_handle, &attr);
    
    if (ret != 0)
    {
        free(mutex);
        return NULL;
    }

    return mutex;
}


/*****************************************************
 * mutex delete function
 * param: mutex --- struct address.
 * return: none.
 ****************************************************/
void util_mutex_delete(util_mutex_t *mutex)
{
    if (!mutex) return;

    pthread_mutex_destroy(mutex->mutex_handle);

    free(mutex);
}

/*****************************************************
 * mutex lock function
 * param: mutex --- struct address.
 * param: timeout --- mutex lock timeout, ms
 * return: int32 --- util_result_t
 ****************************************************/
int32_t util_mutex_lock(util_mutex_t *mutex, int32_t timeout)
{
    if (!mutex) return -1;

        // POSIX平台
    if (timeout == -1)
    {
        // 无限等待
        return pthread_mutex_lock(mutex->mutex_handle);
    }
    else
    {
        // 带超时等待
        struct timespec ts;
        struct timeval tv;

        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + timeout / 1000;
        ts.tv_nsec = tv.tv_usec * 1000 + (timeout % 1000) * 1000000;

        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        int ret = pthread_mutex_timedlock(mutex->mutex_handle, &ts);
        if (ret == ETIMEDOUT)
        {
            return -2; // 超时
        }
        return ret;
    }

    return 0;
}

/*****************************************************
 * mutex unlock function
 * param: mutex --- struct address.
 * return: int32 --- util_result_t
 ****************************************************/
int32_t util_mutex_unlock(util_mutex_t *mutex)
{
    return pthread_mutex_unlock(mutex->mutex_handle);
}

