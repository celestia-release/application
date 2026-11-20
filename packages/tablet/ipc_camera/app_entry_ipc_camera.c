#define LOG_TAG "TABLET_APP_ENTRY_IPC_CAMERA"

#include "app_main.h"
#include "log.h"
#include "tablet_call.h"

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

APP_ENTRY_EXPORT int app_entry_start_ipc_camera(lv_obj_t *main)
{
    if (!main)
        return -1;
    /* Always enter IPC UI regardless of current WiFi state. */
    if (!ipc_call_network_ready())
        log_warn("ipc_camera: network not ready, enter UI anyway");

    if (tablet_call_create(main, 0) != LV_RESULT_OK)
        return -1;
    lv_obj_add_flag(main, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

static int (*const s_keep_app_entry_ipc_camera)(lv_obj_t *) = app_entry_start_ipc_camera;

