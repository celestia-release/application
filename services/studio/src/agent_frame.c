/*
 * agent_frame.c - framebuffer grab + software MJPEG encode for studio
 *
 * Reads /dev/fb0 directly (RGB565/24/32 by var info), decimates 1/1|1/2|1/4
 * and libjpeg-encodes in a timer thread. Never touches the SoC video
 * encoding pipelines - LVGL repaints land in the fb and we read the stable
 * front buffer between pans.
 *
 * Budget note (docs/design/nova/anypi_nova_pc_studio.md): AV200 is an
 * ARM9-class core; default is scale 1/2 + 10 fps to stay near 10% CPU.
 * Raise via SUB {fps, scale} from the PC tool when the device is idle.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* jpeglib.h redefines libc setjmp guards; keep it last */
#include <jpeglib.h>

#include "studio_internal.h"

/*********************
 *      DEFINES
 *********************/
#define SA_FB_DEV          "/dev/fb0"
#define SA_JPEG_QUALITY    55
#define SA_FRAME_MAX_W     2048

/*********************
 *  STATIC VARIABLES
 **********************/
static int s_fb_fd = -1;
static uint8_t *s_fb_mem;
static size_t s_fb_mem_size;
static struct fb_var_screeninfo s_var;
static struct fb_fix_screeninfo s_fix;

/* runtime params */
static pthread_mutex_t s_param_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_fps = 10;
static int s_scale = 2;

static pthread_t s_tid_encoder;
static volatile bool s_encoder_running;
static pthread_mutex_t s_grab_lock = PTHREAD_MUTEX_INITIALIZER;

/* rgb888 staging + jpeg out, reused across frames */
static uint8_t *s_rgb;
static int s_rgb_alloc;

/* Map only bytes the driver backs. line_length * yres_virtual is often
 * larger than smem_len; those extra pages are a valid VMA with no physical
 * memory, and a load there is SIGBUS (BUS_ADRERR). */
static int fb_ensure_map(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;

    if (s_fb_fd < 0)
        return -1;
    if (ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &f) != 0 ||
        ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &v) != 0)
        return -1;
    if (v.bits_per_pixel != 16 && v.bits_per_pixel != 24 && v.bits_per_pixel != 32)
        return -1;
    if (f.line_length <= 0)
        return -1;

    size_t sz = f.smem_len;
    if (sz == 0)
    {
        unsigned vy = v.yres_virtual ? v.yres_virtual : v.yres;
        sz = (size_t)f.line_length * (size_t)vy;
    }
    if (sz == 0)
        return -1;

    if (s_fb_mem && s_fb_mem != MAP_FAILED && sz == s_fb_mem_size &&
        f.smem_start == s_fix.smem_start && f.line_length == s_fix.line_length)
    {
        s_var = v;
        s_fix = f;
        return 0;
    }

    if (s_fb_mem && s_fb_mem != MAP_FAILED)
    {
        munmap(s_fb_mem, s_fb_mem_size);
        s_fb_mem = NULL;
        s_fb_mem_size = 0;
    }

    void *p = mmap(NULL, sz, PROT_READ, MAP_SHARED, s_fb_fd, 0);
    if (p == MAP_FAILED)
    {
        s_fb_mem = NULL;
        s_fb_mem_size = 0;
        return -1;
    }
    s_fb_mem = p;
    s_fb_mem_size = sz;
    s_var = v;
    s_fix = f;
    return 0;
}

/*********************
 *      FUNCTIONS
 **********************/
void sa_frame_get_params(int *fps, int *scale)
{
    pthread_mutex_lock(&s_param_lock);
    if (fps)
        *fps = s_fps;
    if (scale)
        *scale = s_scale;
    pthread_mutex_unlock(&s_param_lock);
}

void sa_frame_set_params(int fps, int scale)
{
    pthread_mutex_lock(&s_param_lock);
    if (fps >= 0 && fps <= 15)
        s_fps = fps ? fps : 1;
    if (scale == 1 || scale == 2 || scale == 4)
        s_scale = scale;
    pthread_mutex_unlock(&s_param_lock);
}

/* open/map the fb and return current geometry; no thread started */
int sa_frame_probe(int *out_w, int *out_h)
{
    struct fb_var_screeninfo v;
    if (s_fb_fd < 0)
    {
        int fd = open(SA_FB_DEV, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &v) != 0)
        {
            close(fd);
            return -1;
        }
        if (out_w)
            *out_w = v.xres;
        if (out_h)
            *out_h = v.yres;
        close(fd);
        return 0;
    }
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &v) != 0)
        return -1;
    if (out_w)
        *out_w = v.xres;
    if (out_h)
        *out_h = v.yres;
    return 0;
}

/* read one fb line into rgb888 honoring bpp and channel offsets.
 * src_end is the first byte past the backed map; never load past it. */
static void convert_line(const uint8_t *src, const uint8_t *src_end, uint8_t *dst, int w, int step)
{
    int bpp = s_var.bits_per_pixel;
    int bpp_bytes = bpp / 8;
    int stride = bpp_bytes * step;
    int roff = s_var.red.offset, rlen = s_var.red.length;
    int goff = s_var.green.offset, glen = s_var.green.length;
    int boff = s_var.blue.offset, blen = s_var.blue.length;

    for (int x = 0; x < w; x++, src += stride)
    {
        uint32_t px = 0;
        /* memcpy, not a widened load: fb pages are device memory, and a
         * short unaligned access there is a bus error. */
        if (bpp_bytes <= 0 || src < s_fb_mem || src + bpp_bytes > src_end)
        {
            memset(dst, 0, (size_t)(w - x) * 3);
            return;
        }
        memcpy(&px, src, (size_t)bpp_bytes);

        /* shift each channel's significant bits to the top 8 */
        uint8_t r = rlen ? (uint8_t)((px >> roff) << (8 - rlen)) : 0;
        uint8_t g = glen ? (uint8_t)((px >> goff) << (8 - glen)) : 0;
        uint8_t b = blen ? (uint8_t)((px >> boff) << (8 - blen)) : 0;
        /*
         * ST7262 stores B,G,R. fb_var offsets come out opposite of the
         * picture on the glass, so orange and blue trade places in JPEG.
         * Emit panel order: JPEG R = fb blue, JPEG B = fb red.
         */
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst += 3;
    }
}

static int grab_locked(uint8_t **jpeg_out, uint32_t *jpeg_len)
{
    *jpeg_out = NULL;
    *jpeg_len = 0;
    if (s_fb_fd < 0 || fb_ensure_map() != 0 || !s_fb_mem)
        return -1;

    /* requery: yoffset tells which buffer is being scanned out */
    struct fb_var_screeninfo v = s_var;
    int w = (int)v.xres, h = (int)v.yres;
    if (w <= 0 || h <= 0 || w > SA_FRAME_MAX_W)
        return -1;

    int scale;
    pthread_mutex_lock(&s_param_lock);
    scale = s_scale;
    pthread_mutex_unlock(&s_param_lock);
    if (scale != 1 && scale != 2 && scale != 4)
        scale = 2;

    int ow = w / scale, oh = h / scale;
    if (ow < 8 || oh < 8)
        return -1;

    size_t need = (size_t)ow * oh * 3;
    if (!s_rgb || s_rgb_alloc < (int)need)
    {
        free(s_rgb);
        s_rgb = malloc(need);
        s_rgb_alloc = (int)need;
        if (!s_rgb)
        {
            s_rgb_alloc = 0;
            return -1;
        }
    }

    long line_bytes = (long)s_fix.line_length;
    int src_bpp = s_var.bits_per_pixel / 8;
    if (line_bytes <= 0 || src_bpp <= 0)
        return -1;
    int yres_v = (int)v.yres_virtual;
    if (yres_v <= 0)
        yres_v = h;
    const uint8_t *src_end = s_fb_mem + s_fb_mem_size;
    for (int y = 0; y < oh; y++)
    {
        int sy = y * scale;
        /* pan offset stays inside the virtual height; the byte offset must
         * also stay inside the backed map. The old fallback still pointed
         * past smem and the load became a bus error. */
        int row = (v.yoffset + sy) % yres_v;
        if (row < 0)
            row += yres_v;
        long src_off = (long)row * line_bytes + (long)v.xoffset * src_bpp;
        long span = (long)ow * (long)scale * src_bpp;
        if (src_off < 0 || span <= 0 ||
            (unsigned long)src_off + (unsigned long)span > s_fb_mem_size)
        {
            src_off = (long)sy * line_bytes;
            if (src_off < 0 ||
                (unsigned long)src_off + (unsigned long)span > s_fb_mem_size)
                return -1;
        }
        convert_line(s_fb_mem + src_off, src_end, s_rgb + (size_t)y * ow * 3, ow, scale);
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char *outbuf = NULL;
    unsigned long outsize = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outbuf, &outsize);
    cinfo.image_width = (JDIMENSION)ow;
    cinfo.image_height = (JDIMENSION)oh;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, SA_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height)
    {
        JSAMPROW row = s_rgb + (size_t)cinfo.next_scanline * ow * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    uint8_t *out = malloc(outsize);
    if (!out)
    {
        free(outbuf);
        return -1;
    }
    memcpy(out, outbuf, outsize);
    free(outbuf);
    *jpeg_out = out;
    *jpeg_len = (uint32_t)outsize;
    return 0;
}

int sa_frame_grab(uint8_t **jpeg_out, uint32_t *jpeg_len)
{
    pthread_mutex_lock(&s_grab_lock);
    int ret = grab_locked(jpeg_out, jpeg_len);
    pthread_mutex_unlock(&s_grab_lock);
    return ret;
}

static void *encoder_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "sa_encoder");
    while (s_encoder_running)
    {
        int fps = 0;
        bool any = false;
        pthread_mutex_lock(&s_param_lock);
        fps = s_fps;
        pthread_mutex_unlock(&s_param_lock);

        /* encode only while someone is watching */
        pthread_mutex_lock(&s_clients_lock);
        for (struct sa_client *c = s_clients; c; c = c->next)
            if (c->active && c->want_frame)
                any = true;
        pthread_mutex_unlock(&s_clients_lock);

        if (any)
        {
            uint8_t *jpeg = NULL;
            uint32_t len = 0;
            if (sa_frame_grab(&jpeg, &len) == 0 && jpeg)
            {
                sa_broadcast_frame(jpeg, len);
                free(jpeg);
            }
        }

        long period_ms = 1000 / (fps > 0 ? fps : 1);
        struct timespec ts = {period_ms / 1000, (period_ms % 1000) * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int sa_frame_init(int *out_w, int *out_h)
{
    if (s_fb_fd >= 0)
    {
        if (sa_frame_probe(out_w, out_h) == 0)
            return 0;
        return -1;
    }

    s_fb_fd = open(SA_FB_DEV, O_RDONLY | O_CLOEXEC);
    if (s_fb_fd < 0)
        return -1;
    if (fb_ensure_map() != 0)
    {
        AG_LOGE("fb map failed: %s", strerror(errno));
        close(s_fb_fd);
        s_fb_fd = -1;
        s_fb_mem = NULL;
        return -1;
    }

    if (out_w)
        *out_w = s_var.xres;
    if (out_h)
        *out_h = s_var.yres;

    if (!s_encoder_running)
    {
        s_encoder_running = true;
        if (pthread_create(&s_tid_encoder, NULL, encoder_thread, NULL) != 0)
        {
            s_encoder_running = false;
            AG_LOGW("encoder thread create failed; SNAPSHOT still works");
        }
        else
            pthread_detach(s_tid_encoder);
    }

    AG_LOGI("frame channel ready: %dx%d bpp=%d", s_var.xres, s_var.yres, s_var.bits_per_pixel);
    return 0;
}
