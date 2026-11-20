#define LOG_TAG "TABLET_APP_ENTRY_SETTING"

#include "tablet_setting.h"

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

APP_ENTRY_EXPORT int app_entry_start_setting(lv_obj_t *main)
{
    if (!main)
        return -1;
    (void)tablet_setting_create(main);
    return 0;
}

static int (*const s_keep_app_entry_setting)(lv_obj_t *) = app_entry_start_setting;

