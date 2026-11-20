#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <sys/resource.h>
#include <stdint.h>
#include <limits.h>

//typedef void (*util_task_entry_func)(void *p1);

typedef struct _util_task_t
{
    pthread_t thread;           // POSIX 线程句柄
    pthread_attr_t attr;        // 线程属性
    util_task_entry_func entry; // 任务入口函数
    void *arg;                  // 任务参数
    char name[32];              // 任务名称
    uint8_t *stack;             // 栈空间指针（仅用于静态任务）
    size_t stack_size;          // 栈大小
    int is_static;              // 是否为静态任务
} inner_task_t;

// 任务入口函数的适配器
static void *task_entry_adapter(void *arg)
{
    inner_task_t *task = (inner_task_t *)arg;

    task->entry(task->arg);

    return NULL;
}

/**
 * @brief 创建任务
 * @param name 任务名称
 * @param prio 任务优先级 (0-255)
 * @param stack_size 栈大小
 * @param entry 任务入口函数
 * @param p1 任务参数
 * @return 成功返回任务指针，失败返回NULL
 */
util_task_t *util_task_create(char *name, uint8_t prio, uint32_t stack_size, 
                             util_task_entry_func entry, void *p1)
 {
    if (!entry) {
        fprintf(stderr, "错误: 任务入口函数不能为空\n");
        return NULL;
    }
    
    // 分配任务结构体
    inner_task_t *task = (inner_task_t *)malloc(sizeof(inner_task_t));
    if (!task) {
        perror("分配任务结构体失败");
        return NULL;
    }
    
    // 初始化任务属性
    if (pthread_attr_init(&task->attr) != 0) {
        perror("初始化线程属性失败");
        free(task);
        return NULL;
    }
    
    // 设置任务名称
    snprintf(task->name, sizeof(task->name), "%s", name ? name : "unnamed");
    
    // 设置栈大小（最小为PTHREAD_STACK_MIN）
    task->stack_size = (stack_size > PTHREAD_STACK_MIN) ? stack_size : PTHREAD_STACK_MIN;
    if (pthread_attr_setstacksize(&task->attr, task->stack_size) != 0) {
        perror("设置栈大小失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }
    
    // 设置调度策略和优先级
    struct sched_param param;
    int policy;
    
    // 获取当前调度策略
    if (pthread_attr_getschedpolicy(&task->attr, &policy) != 0) {
        perror("获取调度策略失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }
    
    // 计算优先级（将0-255映射到系统优先级范围）
    int min_prio = sched_get_priority_min(policy);
    int max_prio = sched_get_priority_max(policy);
    int sys_prio = min_prio + (prio * (max_prio - min_prio)) / 255;
    
    param.sched_priority = sys_prio;
    if (pthread_attr_setschedparam(&task->attr, &param) != 0) {
        perror("设置优先级失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }
    
    // 设置继承调度属性（使用显式设置而不是继承）
    if (pthread_attr_setinheritsched(&task->attr, PTHREAD_EXPLICIT_SCHED) != 0) {
        perror("设置继承调度属性失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }
    
    // 保存任务信息
    task->entry = entry;
    task->arg = p1;
    task->stack = NULL; // 动态分配栈，不管理具体指针
    task->is_static = 0;
    
    // 创建线程
    if (pthread_create(&task->thread, &task->attr, task_entry_adapter, task) != 0) {
        perror("创建线程失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }
    
    printf("创建任务: %s (优先级: %d, 栈大小: %zu)\n", task->name, sys_prio, task->stack_size);
    return (util_task_t*)task;
}

/**
 * @brief 删除任务
 * @param task 任务指针
 */
void util_task_delete(util_task_t *task)
{
    if (!task) return;

    inner_task_t *task_in = (util_task_t *)task;

    // 取消线程
    if (pthread_cancel(task_in->thread) != 0) {
        perror("取消线程失败");
    }
    
    // 等待线程结束
    void *result;
    if (pthread_join(task_in->thread, &result) != 0) {
        perror("等待线程结束失败");
    }
    
    // 销毁属性
    pthread_attr_destroy(&task_in->attr);
    
    // 如果是静态任务，不释放栈空间（由用户管理）
    if (!task_in->is_static) {
        free(task_in);
    }
}

/**
 * @brief 创建消息任务（使用默认设置）
 * @param entry 任务入口函数
 * @param p1 任务参数
 * @return 成功返回任务指针，失败返回NULL
 */
util_task_t *util_msg_task_create(util_task_entry_func entry, void *p1)
{
    // 默认设置
    char *name = "msg_task";
    uint8_t prio = 128; // 中等优先级
    uint32_t stack_size = 8192; // 8KB 栈
    
    return util_task_create(name, prio, stack_size, entry, p1);
}

/**
 * @brief 静态创建任务（使用用户提供的栈空间）
 * @param name 任务名称
 * @param prio 任务优先级
 * @param stack_size 栈大小
 * @param entry 任务入口函数
 * @param p1 任务参数
 * @param stack 用户提供的栈空间
 * @return 成功返回任务指针，失败返回NULL
 */
util_task_t *util_task_create_static(char *name, uint8_t prio, uint32_t stack_size, 
                                    util_task_entry_func entry, void *p1, uint8_t *stack)
{
    if (!stack) {
        fprintf(stderr, "错误: 静态任务必须提供栈空间\n");
        return NULL;
    }
    if (!entry) {
        fprintf(stderr, "错误: 任务入口函数不能为空\n");
        return NULL;
    }

    // 分配任务结构体
    inner_task_t *task = (util_task_t *)malloc(sizeof(util_task_t));
    if (!task)
    {
        perror("分配任务结构体失败");
        return NULL;
    }

    // 初始化任务属性
    if (pthread_attr_init(&task->attr) != 0)
    {
        perror("初始化线程属性失败");
        free(task);
        return NULL;
    }

    // 设置任务名称
    snprintf(task->name, sizeof(task->name), "%s", name ? name : "unnamed_static");

    // 设置栈大小（最小为PTHREAD_STACK_MIN）
    task->stack_size = (stack_size > PTHREAD_STACK_MIN) ? stack_size : PTHREAD_STACK_MIN;

    // 设置用户提供的栈空间
    if (pthread_attr_setstack(&task->attr, stack, task->stack_size) != 0)
    {
        perror("设置栈空间失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }

    // 设置调度策略和优先级
    struct sched_param param;
    int policy;

    // 获取当前调度策略
    if (pthread_attr_getschedpolicy(&task->attr, &policy) != 0)
    {
        perror("获取调度策略失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }

    // 计算优先级（将0-255映射到系统优先级范围）
    int min_prio = sched_get_priority_min(policy);
    int max_prio = sched_get_priority_max(policy);
    int sys_prio = min_prio + (prio * (max_prio - min_prio)) / 255;

    param.sched_priority = sys_prio;

    if (pthread_attr_setschedparam(&task->attr, &param) != 0)
    {
        perror("设置优先级失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }

    // 设置继承调度属性
    if (pthread_attr_setinheritsched(&task->attr, PTHREAD_EXPLICIT_SCHED) != 0)
    {
        perror("设置继承调度属性失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }

    // 保存任务信息
    task->entry = entry;
    task->arg = p1;
    task->stack = stack;
    task->is_static = 1;

    // 创建线程
    if (pthread_create(&task->thread, &task->attr, task_entry_adapter, (void *)task) != 0)
    {
        perror("创建线程失败");
        pthread_attr_destroy(&task->attr);
        free(task);
        return NULL;
    }

    printf("创建静态任务: %s (优先级: %d, 栈大小: %zu)\n", task->name, sys_prio, task->stack_size);

    return (util_task_t *)task;
}

