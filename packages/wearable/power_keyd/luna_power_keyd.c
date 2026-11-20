// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna-only power-key daemon: long-press linux code 90 → GPIO2 PMIC shutdown.
 * Independent of luna_main; does not EVIOCGRAB (shares keyboard evdev).
 * Nova products must not build or install this binary.
 */
#define LOG_TAG "LUNA_POWER_KEYD"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

#define PKD_LINUX_CODE_DEFAULT      90
#define PKD_LONGPRESS_MS_DEFAULT    3000
#define PKD_POST_UP_GRACE_MS        500
#define PKD_BOOT_GRACE_MS           5000
#define PKD_POLL_MS                 50
#define PKD_GPIO_VALUE_PATH         "/sys/class/gpio/gpio2/value"
#define PKD_GPIO_EXPORT_PATH        "/sys/class/gpio/export"
#define PKD_GPIO_DIR_PATH           "/sys/class/gpio/gpio2/direction"
/* KM01A GPIO OUT0 — used when gpio-hog owns GPIO2 (sysfs export busy). */
#define PKD_GPIO_OUT0_PHYS          0x20170014u
#define PKD_GPIO_DIR0_PHYS          0x20170000u
#define PKD_GPIO2_BIT               (1u << 2)

static int s_linux_code = PKD_LINUX_CODE_DEFAULT;
static int s_longpress_ms = PKD_LONGPRESS_MS_DEFAULT;
static int s_armed;
static int s_pressed;
static uint64_t s_down_ms;
static uint64_t s_last_up_ms;
static uint64_t s_start_ms;

static uint64_t pkd_mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void pkd_log(const char *msg)
{
    fprintf(stderr, "[" LOG_TAG "] %s\n", msg);
    fflush(stderr);
}

static int pkd_find_keyboard(char *out_path, size_t out_len)
{
    int i;
    int fd;
    char sys_path[128];
    char name[64];

    if (!out_path || out_len < 20)
        return -1;

    for (i = 0; i < 16; i++) {
        snprintf(sys_path, sizeof(sys_path),
                 "/sys/class/input/event%d/device/name", i);
        fd = open(sys_path, O_RDONLY);
        if (fd < 0)
            continue;
        memset(name, 0, sizeof(name));
        (void)read(fd, name, sizeof(name) - 1);
        close(fd);

        if (strncmp(name, "gpiokeys", 8) == 0 ||
            strncmp(name, "adkeys", 6) == 0 ||
            strncmp(name, "gpio-keys", 9) == 0) {
            snprintf(out_path, out_len, "/dev/input/event%d", i);
            return 0;
        }
    }
    return -1;
}

static int pkd_shutdown_gpio2_mmio(void)
{
    int fd;
    volatile uint32_t *map;
    uint32_t page = PKD_GPIO_DIR0_PHYS & ~0xfffu;
    uint32_t dir_off = PKD_GPIO_DIR0_PHYS - page;
    uint32_t out_off = PKD_GPIO_OUT0_PHYS - page;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;

    map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
    close(fd);
    if (map == MAP_FAILED)
        return -1;

    /* DIR bit=1 => output on KM01A; clear OUT bit2 => power off. */
    map[dir_off / 4] |= PKD_GPIO2_BIT;
    map[out_off / 4] &= ~PKD_GPIO2_BIT;
    munmap((void *)map, 4096);
    pkd_log("gpio2 cleared via /dev/mem (hog/sysfs busy)");
    return 0;
}

static void pkd_shutdown_gpio2(void)
{
    int fd;
    ssize_t n;

    pkd_log("long-press: shutting down via gpio2");
    sync();

    fd = open(PKD_GPIO_VALUE_PATH, O_WRONLY);
    if (fd >= 0) {
        n = write(fd, "0", 1);
        close(fd);
        fprintf(stderr, "[" LOG_TAG "] gpio2 value write ret=%zd\n", n);
        fflush(stderr);
        return;
    }

    fd = open(PKD_GPIO_EXPORT_PATH, O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "2", 1);
        close(fd);
    }
    fd = open(PKD_GPIO_DIR_PATH, O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "out", 3);
        close(fd);
    }
    fd = open(PKD_GPIO_VALUE_PATH, O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "0", 1);
        close(fd);
        pkd_log("gpio2 exported and set to 0");
        return;
    }

    if (pkd_shutdown_gpio2_mmio() != 0)
        pkd_log("gpio2 shutdown failed");
}

static int pkd_boot_grace_active(uint64_t now)
{
    return (s_start_ms != 0 &&
            now < s_start_ms + (uint64_t)PKD_BOOT_GRACE_MS) ? 1 : 0;
}

static void pkd_drain_events(int fd)
{
    struct input_event ev;

    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
        ;
}

static void pkd_on_key(int value)
{
    uint64_t now = pkd_mono_ms();

    /* Ignore boot release / bounce until grace expires. */
    if (pkd_boot_grace_active(now)) {
        s_pressed = 0;
        s_down_ms = 0;
        s_armed = 0;
        return;
    }

    if (value == 1) {
        /* DOWN */
        s_pressed = 1;
        s_down_ms = now;
        return;
    }

    if (value == 0) {
        /* UP — arm only after a real post-boot press cycle. */
        s_pressed = 0;
        s_down_ms = 0;
        s_last_up_ms = now;
        if (!s_armed) {
            s_armed = 1;
            pkd_log("power key UP, shutdown armed");
        }
        return;
    }
    /* value==2 autorepeat: ignore */
}

static void pkd_poll_longpress(void)
{
    uint64_t now;
    uint64_t grace_until;

    if (!s_armed || !s_pressed || s_down_ms == 0)
        return;

    now = pkd_mono_ms();
    if (pkd_boot_grace_active(now))
        return;

    grace_until = s_last_up_ms + (uint64_t)PKD_POST_UP_GRACE_MS;
    if (s_last_up_ms != 0 && s_down_ms < grace_until)
        return;

    if (now - s_down_ms >= (uint64_t)s_longpress_ms) {
        s_pressed = 0;
        pkd_shutdown_gpio2();
        /* If power-off did not take effect, keep running but disarm this hold. */
        s_down_ms = 0;
    }
}

static void pkd_load_settings(const char *path)
{
    FILE *fp;
    long size;
    size_t n;
    char *buf;
    cJSON *root;
    cJSON *input;
    cJSON *power_key;
    cJSON *item;

    if (!path)
        return;
    fp = fopen(path, "rb");
    if (!fp)
        return;
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) <= 0 ||
        size > (1024 * 1024)) {
        fclose(fp);
        return;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return;
    }
    n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';
    root = cJSON_Parse(buf);
    free(buf);
    if (!root)
        return;

    input = cJSON_GetObjectItem(root, "input");
    power_key = cJSON_IsObject(input) ?
                cJSON_GetObjectItem(input, "power_key") : NULL;
    if (cJSON_IsObject(power_key)) {
        item = cJSON_GetObjectItem(power_key, "linux_code");
        if (cJSON_IsNumber(item))
            s_linux_code = item->valueint;
        item = cJSON_GetObjectItem(power_key, "longpress_ms");
        if (cJSON_IsNumber(item))
            s_longpress_ms = item->valueint;
    }
    cJSON_Delete(root);

    if (s_linux_code <= 0)
        s_linux_code = PKD_LINUX_CODE_DEFAULT;
    if (s_longpress_ms < 500)
        s_longpress_ms = PKD_LONGPRESS_MS_DEFAULT;
}

int main(int argc, char **argv)
{
    char dev_path[64];
    int fd = -1;
    int guard;
    const char *cfg = "/etc/config/settings.json";

    (void)argc;
    (void)argv;

    if (access(cfg, R_OK) == 0)
        pkd_load_settings(cfg);

    fprintf(stderr, "[" LOG_TAG "] start linux_code=%d longpress_ms=%d\n",
            s_linux_code, s_longpress_ms);
    fflush(stderr);

    /* Keyboard node may appear after gpiokeys module; retry briefly. */
    for (guard = 0; guard < 50; guard++) {
        if (pkd_find_keyboard(dev_path, sizeof(dev_path)) == 0)
            break;
        usleep(100 * 1000);
        if (guard == 49) {
            pkd_log("keyboard device not found, exit");
            return 1;
        }
    }

    fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[" LOG_TAG "] open %s failed errno=%d\n",
                dev_path, errno);
        return 1;
    }
    s_start_ms = pkd_mono_ms();
    pkd_drain_events(fd);
    s_armed = 0;
    s_pressed = 0;
    s_down_ms = 0;
    fprintf(stderr, "[" LOG_TAG "] listening %s (no grab, boot_grace=%dms)\n",
            dev_path, PKD_BOOT_GRACE_MS);
    fflush(stderr);

    for (;;) {
        struct pollfd pfd;
        int pret;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pret = poll(&pfd, 1, PKD_POLL_MS);
        if (pret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pret > 0 && (pfd.revents & POLLIN)) {
            struct input_event ev;
            ssize_t n;

            while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                if (ev.type != EV_KEY)
                    continue;
                if ((int)ev.code != s_linux_code)
                    continue;
                pkd_on_key(ev.value);
            }
        }

        pkd_poll_longpress();
    }

    close(fd);
    return 0;
}
