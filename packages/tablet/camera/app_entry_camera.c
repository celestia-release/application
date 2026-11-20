#define LOG_TAG "TABLET_APP_ENTRY_CAMERA"

#include "tablet_camera.h"
#include "lvgl.h"

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

APP_ENTRY_EXPORT int app_entry_start_camera(lv_obj_t *main)
{
    if (!main)
        return -1;
    lv_obj_add_flag(main, LV_OBJ_FLAG_HIDDEN);
    (void)tablet_camera_create(main);
    return 0;
}

static int (*const s_keep_app_entry_camera)(lv_obj_t *) = app_entry_start_camera;

