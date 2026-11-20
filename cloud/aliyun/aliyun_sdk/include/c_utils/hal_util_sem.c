#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <stdint.h>

// 信号量结构体定义
typedef struct _util_sem_t
{
    sem_t sem;              // POSIX信号量

    uint32_t max_count;         // 最大计数值
    uint32_t init_count;        // 初始计数值
} inner_sem_t;


/*****************************************************
 * sem create function
 * param: max_count --- max sem count.
 * param: init_count --- init sem count.
 * return: util_sem_t *---struct adress
 ****************************************************/
util_sem_t * util_sem_create(uint32_t max_count, uint32_t init_count)
{
    // 分配内存
    inner_sem_t *sem = malloc(sizeof(inner_sem_t));
    
    sem_init(&sem->sem, 0, init_count);

    return (util_sem_t *)sem;
}

/*****************************************************
 * sem delete function
 * param: sem --- struct adress.
 * return: none
 ****************************************************/
void util_sem_delete(util_sem_t *sem)
{
    inner_sem_t *sem_in = (inner_sem_t *)sem;
    sem_destroy(&sem_in->sem);

    free(sem_in);
}

/*****************************************************
 * sem take function
 * param: sem --- struct adress.
 * param: timeout --- sem take timeout, ms
 * return: int32 --- util_result_t
 ****************************************************/
int32_t util_sem_take(util_sem_t *sem, int32_t timeout)
{
    inner_sem_t *sem_in = (inner_sem_t *)sem;
    if (timeout <= 0)
    {
        while (sem_wait(&sem_in->sem) && errno == EINTR);
        return 0;
    }
    else
    {
        // 计算绝对超时时间
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);

        ts.tv_sec += timeout / 1000;
        ts.tv_nsec += (timeout % 1000) * 1000000;

        return sem_timedwait(&sem_in->sem, &ts) == 0 ? 0 : -1;
    }

    return 0;
}

/*****************************************************
 * sem give function
 * param: sem --- struct adress.
 * return: int32 --- util_result_t
 ****************************************************/
int32_t util_sem_give(util_sem_t * sem)
{
    inner_sem_t *sem_in = (inner_sem_t *)sem;
    return sem_post(&sem_in->sem) == 0 ? 0 : -1;
}

/*****************************************************
 * sem reset function
 * param: sem --- struct adress.
 * return: int32 --- util_result_t
 ****************************************************/
int32_t util_sem_reset(util_sem_t *sem)
{
    if (!sem) return -1;

    inner_sem_t *sem_in = (inner_sem_t *)sem;
        
    // 保存原始参数
    uint32_t max = sem_in->max_count;
    uint32_t init = sem_in->init_count;
    
    // 删除旧信号量
    util_sem_delete(sem);
    
    // 创建新信号量
    util_sem_t *new_sem = util_sem_create(max, init);
    if (!new_sem) return -1;
    
    // 复制内容到原结构体
    *sem = *new_sem;

    return 0;
}

/*****************************************************
 * msg sem create function
 * return: util_sem_t *---struct adress
 ****************************************************/
util_sem_t * util_msg_sem_create(void)
{
    return util_sem_create(1, 0);
}



