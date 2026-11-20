// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Scheme B: FreeType / libpng are NOT needed for Home first frame (bitmap fonts,
 * QOI wallpaper). Do not DT_NEEDED them — provide trampolines that dlopen the
 * shared libraries on first use (or via app_main_load_deferred_gui_libs after Home).
 */
#define LOG_TAG "APP MAIN"

#include <stddef.h>
#include <dlfcn.h>
#include <pthread.h>

#include "config.h"
#include "log.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_IMAGE_H
#include FT_MODULE_H

#include "png.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void *s_ft;
static void *s_png;

static void *sym_ft(const char *name)
{
    static const char *const candidates[] = {
        "libfreetype.so",
        "/usr/lib/libfreetype.so",
        "/mnt/install/lib/libfreetype.so",
        NULL
    };
    void *p;
    const char *err = NULL;
    int i;

    if (!s_ft) {
        for (i = 0; candidates[i]; i++) {
            s_ft = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
            if (s_ft) {
                log_info("dlopen %s ok", candidates[i]);
                break;
            }
            err = dlerror();
        }
        if (!s_ft)
            log_error("dlopen libfreetype failed (last try): %s", err ? err : "unknown");
    }
    if (!s_ft)
        return NULL;
    p = dlsym(s_ft, name);
    if (!p)
        log_error("dlsym FT %s: %s", name, dlerror());
    return p;
}

static void *sym_png(const char *name)
{
    static const char *const candidates[] = {
        "libpng16.so",
        "libpng.so",
        "/usr/lib/libpng16.so",
        "/usr/lib/libpng.so",
        "/mnt/install/lib/libpng16.so",
        "/mnt/install/lib/libpng.so",
        NULL
    };
    void *p;
    const char *err = NULL;
    int i;

    if (!s_png) {
        for (i = 0; candidates[i]; i++) {
            s_png = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
            if (s_png) {
                log_info("dlopen %s ok", candidates[i]);
                break;
            }
            err = dlerror();
        }
        if (!s_png)
            log_error("dlopen libpng failed (last=%s): %s",
                      candidates[i ? i - 1 : 0], err ? err : "unknown");
    }
    if (!s_png)
        return NULL;
    p = dlsym(s_png, name);
    if (!p)
        log_error("dlsym PNG %s: %s", name, dlerror());
    return p;
}

int app_main_load_deferred_gui_libs(void)
{
    int rc = 0;

    pthread_mutex_lock(&s_lock);
    if (!sym_ft("FT_Init_FreeType"))
        rc = -1;
    if (!sym_png("png_image_begin_read_from_memory"))
        rc = -1;
    pthread_mutex_unlock(&s_lock);
    if (rc == 0)
        log_info("deferred GUI libs ready (libfreetype + libpng)");
    return rc;
}

#define FT_WRAP_0(ret, name, fail) \
ret name(void) \
{ \
    typedef ret (*fn_t)(void); \
    fn_t fn; \
    pthread_mutex_lock(&s_lock); \
    fn = (fn_t)sym_ft(#name); \
    pthread_mutex_unlock(&s_lock); \
    return fn ? fn() : (fail); \
}

#define FT_WRAP(ret, name, fail, args_decl, args_call) \
ret name args_decl \
{ \
    typedef ret (*fn_t) args_decl; \
    fn_t fn; \
    pthread_mutex_lock(&s_lock); \
    fn = (fn_t)sym_ft(#name); \
    pthread_mutex_unlock(&s_lock); \
    return fn ? fn args_call : (fail); \
}

#define FT_WRAP_V(name, args_decl, args_call) \
void name args_decl \
{ \
    typedef void (*fn_t) args_decl; \
    fn_t fn; \
    pthread_mutex_lock(&s_lock); \
    fn = (fn_t)sym_ft(#name); \
    pthread_mutex_unlock(&s_lock); \
    if (fn) \
        fn args_call; \
}

FT_WRAP(FT_Error, FT_Init_FreeType, (FT_Error)-1, (FT_Library *alibrary), (alibrary))
FT_WRAP(FT_Error, FT_Done_FreeType, (FT_Error)-1, (FT_Library library), (library))
FT_WRAP(FT_Error, FT_New_Face, (FT_Error)-1,
        (FT_Library library, const char *filepathname, FT_Long face_index, FT_Face *aface),
        (library, filepathname, face_index, aface))
FT_WRAP(FT_Error, FT_Done_Face, (FT_Error)-1, (FT_Face face), (face))
FT_WRAP(FT_Error, FT_Set_Pixel_Sizes, (FT_Error)-1,
        (FT_Face face, FT_UInt pixel_width, FT_UInt pixel_height),
        (face, pixel_width, pixel_height))
FT_WRAP(FT_Error, FT_Select_Size, (FT_Error)-1, (FT_Face face, FT_Int strike_index),
        (face, strike_index))
FT_WRAP(FT_Error, FT_Load_Glyph, (FT_Error)-1,
        (FT_Face face, FT_UInt glyph_index, FT_Int32 load_flags),
        (face, glyph_index, load_flags))
FT_WRAP(FT_Error, FT_Render_Glyph, (FT_Error)-1,
        (FT_GlyphSlot slot, FT_Render_Mode render_mode), (slot, render_mode))
FT_WRAP(FT_UInt, FT_Get_Char_Index, (FT_UInt)0,
        (FT_Face face, FT_ULong charcode), (face, charcode))
FT_WRAP(FT_Error, FT_Get_Kerning, (FT_Error)-1,
        (FT_Face face, FT_UInt left_glyph, FT_UInt right_glyph, FT_UInt kern_mode,
         FT_Vector *akerning),
        (face, left_glyph, right_glyph, kern_mode, akerning))
FT_WRAP(FT_Error, FT_Get_Glyph, (FT_Error)-1, (FT_GlyphSlot slot, FT_Glyph *aglyph),
        (slot, aglyph))
FT_WRAP_V(FT_Done_Glyph, (FT_Glyph glyph), (glyph))
FT_WRAP_V(FT_Set_Transform,
          (FT_Face face, FT_Matrix *matrix, FT_Vector *delta), (face, matrix, delta))
FT_WRAP(FT_Long, FT_MulFix, (FT_Long)0, (FT_Long a, FT_Long b), (a, b))
FT_WRAP(FT_Error, FT_Outline_Embolden, (FT_Error)-1,
        (FT_Outline *outline, FT_Pos strength), (outline, strength))
FT_WRAP(FT_Error, FT_Outline_Decompose, (FT_Error)-1,
        (FT_Outline *outline, const FT_Outline_Funcs *func_interface, void *user),
        (outline, func_interface, user))
FT_WRAP(FT_Error, FT_Stroker_New, (FT_Error)-1,
        (FT_Library library, FT_Stroker *astroker), (library, astroker))
FT_WRAP_V(FT_Stroker_Set,
          (FT_Stroker stroker, FT_Fixed radius, FT_Stroker_LineCap line_cap,
           FT_Stroker_LineJoin line_join, FT_Fixed miter_limit),
          (stroker, radius, line_cap, line_join, miter_limit))
FT_WRAP_V(FT_Stroker_Done, (FT_Stroker stroker), (stroker))
FT_WRAP(FT_Error, FT_Glyph_StrokeBorder, (FT_Error)-1,
        (FT_Glyph *pglyph, FT_Stroker stroker, FT_Bool inside, FT_Bool destroy),
        (pglyph, stroker, inside, destroy))

int png_image_begin_read_from_memory(png_imagep image, png_const_voidp memory, size_t size)
{
    typedef int (*fn_t)(png_imagep, png_const_voidp, size_t);
    fn_t fn;

    pthread_mutex_lock(&s_lock);
    fn = (fn_t)sym_png("png_image_begin_read_from_memory");
    pthread_mutex_unlock(&s_lock);
    return fn ? fn(image, memory, size) : 0;
}

int png_image_finish_read(png_imagep image, png_const_colorp background, void *buffer,
                          png_int_32 row_stride, void *colormap)
{
    typedef int (*fn_t)(png_imagep, png_const_colorp, void *, png_int_32, void *);
    fn_t fn;

    pthread_mutex_lock(&s_lock);
    fn = (fn_t)sym_png("png_image_finish_read");
    pthread_mutex_unlock(&s_lock);
    return fn ? fn(image, background, buffer, row_stride, colormap) : 0;
}

void png_image_free(png_imagep image)
{
    typedef void (*fn_t)(png_imagep);
    fn_t fn;

    pthread_mutex_lock(&s_lock);
    fn = (fn_t)sym_png("png_image_free");
    pthread_mutex_unlock(&s_lock);
    if (fn)
        fn(image);
}
