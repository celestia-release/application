#define LOG_TAG "TABLET_APP_ENTRY_AUDIO_ALGO"

#include "tablet_audio_algo.h"

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

APP_ENTRY_EXPORT int app_entry_start_audio_algo(lv_obj_t *main)
{
    if (!main)
        return -1;
    lv_result_t ret = tablet_audio_algo_create(main);
    if (ret != LV_RESULT_OK) {
        return -1;
    }
    lv_obj_add_flag(main, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

static int (*const s_keep_app_entry_audio_algo)(lv_obj_t *) = app_entry_start_audio_algo;

