#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

char *storage_file = "/etc/config/aliyun/config_file";
char *storage_path = "/etc/config/aliyun";

static void print_hex(const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0)  // 每 16 字节换行
            printf("\n");
    }
    printf("\n");
}

/**
 * @brief擦除存储器
 * 
 * 该函数用于擦除存储器中的所有数据。在调用此函数之前，应确保不再需要存储器中的任何信息，
 * 因为擦除操作将删除所有数据，且此操作不可逆。
 * 
 * @return int32_t 返回擦除操作的结果。如果返回值为0，表示擦除成功；如果返回值非0，表示擦除过程中出现错误。
 */
int32_t util_storage_erase(void)
{
    FILE *fp = fopen(storage_file,"w+");

    if(fp)
    {
        fclose(fp);
        return 0;
    }

    return -1;
}

/**
 * @brief存储数据到存储器
 * 
 * 该函数将指定的数据存储到存储器中。在调用此函数之前，应确保数据的正确性和完整性，
 * 因为存储操作将覆盖存储器中的现有数据。
 * 
 * @param data 指向要存储的数据的指针。数据类型为uint8_t，即无符号的8位整数。
 * @param size 要存储的数据的大小，以字节为单位。数据类型为uint32_t，即无符号的32位整数。
 * @return int32_t 返回存储操作的结果。如果返回值为0，表示存储成功；如果返回值非0，表示存储过程中出现错误。
 */
int32_t util_storage_storage(uint8_t *data, uint32_t size)
{
    if(access(storage_path, F_OK) != 0)
    {
        mkdir(storage_path,0755);
    }

    FILE *fp = fopen(storage_file,"w+");

    if(fp)
    {
        fwrite(data,1,size,fp);
        fclose(fp);
    }
    return 0;
}

/**
 * @brief从存储器加载数据
 * 
 * 该函数从存储器中加载指定大小的数据。在调用此函数之前，应确保提供的数据指针指向的内存区域足够大，
 * 以容纳从存储器加载的数据。
 * 
 * @param data 指向用于存储从存储器加载的数据的缓冲区的指针。数据类型为uint8_t，即无符号的8位整数。
 * @param size 要加载的数据的大小，以字节为单位。数据类型为uint32_t，即无符号的32位整数。
 * @return int32_t 返回加载操作的结果。如果返回值为0，表示加载成功；如果返回值非0，表示加载过程中出现错误。
 */
int32_t util_storage_load(uint8_t *data, uint32_t size)
{
    FILE *fp = fopen(storage_file,"r");

    if(fp)
    {
        fread(data,1,size,fp);
        fclose(fp);
        return 0;
    }

    return -1;
}

