/*
 * Copyright 2026 Alibaba Group Holding Ltd.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http: *www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef APPLICATION_HAL_UTIL_FS_H__
#define APPLICATION_HAL_UTIL_FS_H__

#include "c_utils.h"

#if CONFIG_PLATFORM_LINUX
#ifndef O_RDONLY
#define O_RDONLY    0x0000    
#endif
#ifndef O_WRONLY
#define O_WRONLY    0x0001      
#endif
#ifndef O_RDWR
#define O_RDWR      0x0002      
#endif
#ifndef O_CREAT
#define O_CREAT     0x0040      
#endif
#ifndef O_TRUNC
#define O_TRUNC     0x0200     
#endif
#endif

#ifndef O_RDONLY
#define O_RDONLY    0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY    0x0001
#endif
#ifndef O_RDWR
#define O_RDWR      0x0002
#endif
#ifndef O_CREAT
#define O_CREAT     0x0200
#endif
#ifndef O_TRUNC
#define O_TRUNC     0x0400
#endif

#ifndef SEEK_SET
#define SEEK_SET    0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR    1
#endif
#ifndef SEEK_END
#define SEEK_END    2
#endif

#ifndef F_OK
#define F_OK    0
#endif
#ifndef X_OK
#define X_OK    1
#endif
#ifndef W_OK
#define W_OK    2
#endif
#ifndef R_OK
#define R_OK    4
#endif

#define util_dir_t void
#define util_dirent_t struct dirent

FILE *util_open(const char *path, int32_t flags);
int32_t util_close(FILE *fd);
int32_t util_read(FILE *fd, void *buf, size_t nbytes);
int32_t util_write(FILE *fd, const void *buf, size_t nbytes);
int32_t util_lseek(FILE *fd, int32_t offset, int32_t whence);
int32_t util_sync(FILE *fd);
int32_t util_remove(const char *path);
int32_t util_file_rewind(FILE *fd);
int32_t util_fsize(FILE *fd);
int32_t util_rename(const char *oldpath, const char *newpath);

util_dir_t *util_opendir(const char *path);
int32_t util_closedir(util_dir_t *dir);
util_dirent_t *util_readdir(util_dir_t *dir);
int32_t util_mkdir(const char *path);
int32_t util_rmdir(const char *path);
int32_t util_rmdir_force(const char *path);

int32_t util_access(const char *path, int32_t mode);

int32_t util_fs_init(void);
void util_fs_test(void);

#endif
