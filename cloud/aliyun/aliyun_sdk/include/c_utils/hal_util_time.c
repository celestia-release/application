#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>

int init_flag = 0;
/**
 * 获取当前时间戳（毫秒级）
 * 
 * 此函数用于获取当前的时间戳，精确到毫秒该时间戳通常用于计算时间差、
 * 记录事件发生时间等场景
 * 
 * 返回：当前时间戳（毫秒级）
 */
int64_t util_now_ms(void)
{
	struct timespec sttimespec;

	clock_gettime(CLOCK_MONOTONIC,&sttimespec);

	int64_t llt = (int64_t)(sttimespec.tv_sec) * 1000 + (int64_t)sttimespec.tv_nsec / 1000000;

	return llt;
}

/**
 * 毫秒级睡眠函数
 * 
 * 此函数使当前线程暂停执行指定毫秒数，用于控制程序执行节奏、等待事件发生等
 * 
 * 参数 ms：需要暂停的毫秒数
 */
void util_msleep(uint32_t ms)
{
    usleep(ms * 1000);
}

/**
 * 获取当前时间戳
 * 
 * 此函数用于获取当前的时间戳，即从1970年1月1日00:00:00 UTC开始到现在的毫秒数
 * 它没有输入参数，返回一个int64_t类型的值，代表当前的时间戳
 * 
 * @return int64_t 当前时间戳，单位为毫秒
 */
int64_t util_get_timestamp(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
    int64_t llt = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
    return llt;
}

int64_t util_get_timestamp_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	int64_t llt = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;

	return llt;
}

/**
 * 检查时间戳功能是否已初始化
 * 
 * 此函数用于检查时间戳相关功能是否已经初始化如果返回真（非零），则表示
 * 时间戳功能可用；如果返回假（零），则可能需要进行初始化操作或者避免使用时间戳功能
 * 
 * 返回：如果时间戳功能已初始化，则返回非零，否则返回零
 */
uint8_t util_timestamp_inited(void)
{
	time_t t= time(NULL);

    struct tm *value = localtime(&t);

    if(1900 + value->tm_year >= 2025)
    {
        return 1;
    }

    return 0;
}

