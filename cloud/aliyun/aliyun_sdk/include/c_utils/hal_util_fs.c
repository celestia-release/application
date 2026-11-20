#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include "hal_util_fs.h"
#include "limits.h"

// 文件打开标志转换
static const char* flags_to_mode(int32_t flags) {
    if (flags & O_RDWR) {
        if (flags & O_CREAT) {
            if (flags & O_TRUNC) return "w+";
            if (flags & O_APPEND) return "a+";
            return "r+";
        }
        return "r+";
    }
    if (flags & O_WRONLY) {
        if (flags & O_CREAT) {
            if (flags & O_TRUNC) return "w";
            if (flags & O_APPEND) return "a";
            return "w";
        }
        return "w";
    }
    return "r"; // 默认只读
}

FILE *util_open(const char *path, int32_t flags)
{
    const char *mode = flags_to_mode(flags);
    FILE *fp = fopen(path, mode);
    
    if (!fp)
    {
        perror("util_open failed");
        return NULL;
    }
    return fp;
}

int32_t util_close(FILE *fd)
{
    if (!fd) return -1;

    return fclose(fd);
}

int32_t util_read(FILE *fd, void *buf, size_t nbytes)
{
    if (!fd || !buf) return -1;

    size_t n = fread(buf, 1, nbytes, fd);
    
    if (ferror(fd))
    {
        perror("util_read failed");
        return -1;
    }

    return (int32_t)n;
}

int32_t util_write(FILE *fd, const void *buf, size_t nbytes)
{
    if (!fd || !buf) return -1;

    size_t n = fwrite(buf, 1, nbytes, fd);

    if (ferror(fd))
    {
        perror("util_write failed");
        return -1;
    }

    return (int32_t)n;
}

int32_t util_lseek(FILE *fd, int32_t offset, int32_t whence)
{
    if (!fd) return -1;

    if (fseek(fd, offset, whence) != 0)
    {
        perror("util_lseek failed");
        return -1;
    }

    return ftell(fd);
}
int32_t util_sync(FILE *fd)
{
    if (!fd) return -1;
    
    // 刷新用户空间缓冲区
    if (fflush(fd) != 0)
    {
        perror("util_sync fflush failed");
        return -1;
    }
    
    // 同步到磁盘
    int fp = fileno(fd);
    if (fsync(fp) != 0)
    {
        perror("util_sync fsync failed");
        return -1;
    }
    
    return 0;
}


int32_t util_remove(const char *path)
{
    if (!path) return -1;

    return remove(path);
}


int32_t util_file_rewind(FILE *fd)
{
    if (!fd) return -1;

    rewind(fd);

    return 0;
}

int32_t util_fsize(FILE *fd)
{
    if (!fd) return -1;
     
     // 保存当前位置
     long current = ftell(fd);
 
     if (current == -1) return -1;
     
     // 定位到文件末尾
     if (fseek(fd, 0, SEEK_END) != 0) return -1;
     
     // 获取文件大小
     long size = ftell(fd);
     
     // 恢复原位置
     fseek(fd, current, SEEK_SET);
     
     return (int32_t)size;
}
int32_t util_rename(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath) return -1;

    return rename(oldpath, newpath);
}

util_dir_t *util_opendir(const char *path)
{
    if (!path) return NULL;

    return opendir(path);
}

int32_t util_closedir(util_dir_t *dir)
{
    if (!dir) return -1;

    return closedir(dir);
}

util_dirent_t *util_readdir(util_dir_t *dir)
{
    if (!dir) return NULL;

    return readdir(dir);
}

int32_t util_mkdir(const char *path)
{
    if (!path) return -1;

    return mkdir(path, 0755); // rwxr-xr-x
}

int32_t util_rmdir(const char *path)
{
    if (!path) return -1;

    return rmdir(path);
}

int32_t util_rmdir_force(const char *path)
{
    if (!path) return -1;
    
    DIR *dir = opendir(path);
    if (!dir)
    {
        // 如果不是目录，尝试删除文件
        return unlink(path) == 0 ? 0 : -1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        // 跳过 "." 和 ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 构建子路径
        char subpath[PATH_MAX];
        snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);
        
        // 递归删除子项
        if (util_rmdir_force(subpath) != 0) {
            closedir(dir);
            return -1;
        }
    }
    
    closedir(dir);
    return rmdir(path);
}


int32_t util_access(const char *path, int32_t mode)
{
    if (!path) return -1;

    return access(path, mode);
}

int32_t util_fs_init(void)
{
    return 0;
}

void util_fs_test(void)
{
    const char *filename = "/tmp/test_file.txt";
    
    printf("===== 文件操作测试 =====\n");
    
    // 创建并写入文件
    FILE *fp = util_open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (!fp) {
        perror("创建文件失败");
        return;
    }
    
    const char *content = "这是一个测试文件\n用于验证文件系统API功能\n";
    int len = strlen(content);
    
    if (util_write(fp, content, len) != len) {
        perror("写入文件失败");
        util_close(fp);
        return;
    }
    
    printf("成功写入 %d 字节到文件\n", len);
    
    // 同步文件
    if (util_sync(fp) != 0) {
        perror("文件同步失败");
    }
    
    // 获取文件大小
    int size = util_fsize(fp);
    printf("文件大小: %d 字节\n", size);
    
    util_close(fp);
    
    // 读取文件
    fp = util_open(filename, O_RDONLY);
    if (!fp) {
        perror("打开文件失败");
        return;
    }
    
    char buffer[256];
    int bytes_read = util_read(fp, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
        perror("读取文件失败");
    } else {
        buffer[bytes_read] = '\0';
        printf("文件内容:\n%s\n", buffer);
    }
    
    // 测试文件定位
    if (util_lseek(fp, 10, SEEK_SET) != 10) {
        perror("文件定位失败");
    }
    
    bytes_read = util_read(fp, buffer, 5);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("从位置10读取5字节: %s\n", buffer);
    }
    
    util_close(fp);
    
    // 重命名文件
    const char *new_filename = "/tmp/renamed_test_file.txt";
    if (util_rename(filename, new_filename) == 0) {
        printf("文件重命名成功: %s -> %s\n", filename, new_filename);
    } else {
        perror("文件重命名失败");
    }
    
    // 删除文件
    if (util_remove(new_filename) == 0) {
        printf("文件删除成功\n");
    } else {
        perror("文件删除失败");
    }
}



