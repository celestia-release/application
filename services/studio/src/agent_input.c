/*
 * agent_input.c - inject PC pointer events into the board touchscreen evdev node.
 *
 * The panel reader (hardware/input/evdev/touch.c, raw path) consumes
 * ABS_X/ABS_Y/BTN_TOUCH from that node and scales them onto the LCD.
 * Writing the same node from here is what the kernel delivers to that reader.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "studio_internal.h"

static int s_fd = -1;
static int s_abs_x = 799;
static int s_abs_y = 479;
static int s_ready;

static int name_is_touch(const char *name)
{
    if (!name || !name[0])
        return 0;
    return strstr(name, "touch") || strstr(name, "Touch") || strstr(name, "TOUCH") ||
           strstr(name, "gt9") || strstr(name, "GT9") ||
           strstr(name, "goodix") || strstr(name, "Goodix");
}

static int open_touch(void)
{
    DIR *d;
    struct dirent *e;
    char best[64] = {0};

    if (s_fd >= 0)
        return 0;
    d = opendir("/sys/class/input");
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
    {
        int idx;
        char path[160], name[128];
        FILE *fp;
        if (sscanf(e->d_name, "event%d", &idx) != 1)
            continue;
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", e->d_name);
        fp = fopen(path, "r");
        if (!fp)
            continue;
        if (!fgets(name, sizeof(name), fp))
            name[0] = 0;
        fclose(fp);
        if (!name_is_touch(name))
            continue;
        snprintf(best, sizeof(best), "/dev/input/event%d", idx);
        break;
    }
    closedir(d);
    if (!best[0])
    {
        AG_LOGW("input: no touch event node");
        return -1;
    }
    s_fd = open(best, O_WRONLY | O_CLOEXEC);
    if (s_fd < 0)
    {
        AG_LOGW("input: open %s failed: %s", best, strerror(errno));
        return -1;
    }
    {
        struct input_absinfo ax;
        if (ioctl(s_fd, EVIOCGABS(ABS_X), &ax) == 0 && ax.maximum > 0)
            s_abs_x = ax.maximum;
        if (ioctl(s_fd, EVIOCGABS(ABS_Y), &ax) == 0 && ax.maximum > 0)
            s_abs_y = ax.maximum;
    }
    s_ready = 1;
    AG_LOGI("input: touch %s abs %d x %d", best, s_abs_x, s_abs_y);
    return 0;
}

static void emit(unsigned short type, unsigned short code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(s_fd, &ev, sizeof(ev)) < 0 && errno != EAGAIN)
    {
        close(s_fd);
        s_fd = -1;
        s_ready = 0;
    }
}

int sa_input_touch(int x, int y, int down, int sw, int sh)
{
    int ax, ay;
    if (open_touch() != 0)
        return -1;
    if (sw < 2)
        sw = 2;
    if (sh < 2)
        sh = 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= sw)
        x = sw - 1;
    if (y >= sh)
        y = sh - 1;
    ax = (int)((long)x * s_abs_x / (sw - 1));
    ay = (int)((long)y * s_abs_y / (sh - 1));

    emit(EV_ABS, ABS_X, ax);
    emit(EV_ABS, ABS_Y, ay);
    emit(EV_ABS, ABS_MT_POSITION_X, ax);
    emit(EV_ABS, ABS_MT_POSITION_Y, ay);
    emit(EV_ABS, ABS_MT_TRACKING_ID, down ? 1 : -1);
    emit(EV_KEY, BTN_TOUCH, down ? 1 : 0);
    emit(EV_ABS, ABS_PRESSURE, down ? 50 : 0);
    emit(EV_SYN, SYN_REPORT, 0);
    return s_fd >= 0 ? 0 : -1;
}
