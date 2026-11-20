#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

// 随机数生成器结构体
typedef struct {
    uint32_t seed;      // 当前种子值
    uint32_t a;         // 乘数
    uint32_t c;         // 增量
    uint32_t m;         // 模数
    uint8_t initialized; // 初始化标志
} RandGenerator;


// 全局随机数生成器实例
static RandGenerator rng = {0};

// 初始化随机数生成器
void rand_init(uint32_t seed)
{
    // 如果seed为0，使用时间作为种子
    if (seed == 0) {
        seed = (uint32_t)time(NULL);
    }
    
    // 设置LCG参数 (使用glibc的参数)
    rng.a = 1103515245;
    rng.c = 12345;
    rng.m = 0x7FFFFFFF; // 2^31 - 1
    
    // 初始种子处理
    rng.seed = (seed ^ rng.a) * rng.c;
    rng.initialized = 1;
}

// 生成32位无符号随机数
uint32_t rand_generate(void)
{
    // 如果未初始化，自动初始化
    if (!rng.initialized)
    {
        rand_init(0);
    }
    
    // 线性同余算法: seed = (a * seed + c) mod m
    rng.seed = (uint32_t)((uint64_t)rng.a * rng.seed + rng.c) % rng.m;
    
    // 为了更好的随机性分布，应用Xorshift处理
    rng.seed ^= rng.seed << 13;
    rng.seed ^= rng.seed >> 17;
    rng.seed ^= rng.seed << 5;
    
    return rng.seed;
}
/**
 * 初始化随机数生成器
 * 
 * @param seed 用于初始化随机数生成器的种子值
 * 
 * @return 返回初始化结果，0表示成功，非0表示失败
 * 
 * 此函数通过对随机数生成器进行初始化，以确保后续生成的随机数序列具有良好的随机性
 * 种子值的选择对生成的随机数序列有重要影响，相同的种子值会生成相同的随机数序列
 */
int32_t util_random_init(uint32_t seed)
{
    rand_init(seed);
    return 0;
}

/**
 * 生成一个随机数
 * 
 * @return 返回生成的随机数
 * 
 * 在调用此函数之前，应确保随机数生成器已经通过util_random_init函数成功初始化
 * 此函数生成的随机数是基于初始化时提供的种子值产生的
 */
uint32_t util_random(void)
{
    return rand_generate();
}

