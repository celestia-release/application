// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Headless stubs for symbols referenced by libnebula_protocol.so but absent on
 * anypi_luna_dev (CONFIG_FW_MULTIMEDIA_MEDIA_PLAYER=n, CONFIG_HW_DISPLAY=n).
 * Linked into libipc_cam_app.so; ipc_cam must be dlopen'd with RTLD_GLOBAL so
 * subsequent dlopen(libnebula_protocol) can resolve these.
 */
#define LOG_TAG "IPCAMERA_NEBULA_STUBS"

#include "log.h"

#include <stddef.h>

#define STUB_EXPORT __attribute__((visibility("default"))) __attribute__((used))

STUB_EXPORT int media_player_open_real(void *video_info, void *audio_info)
{
    (void)video_info;
    (void)audio_info;
    log_warn("media_player_open_real stub (no media player on this product)");
    return -1;
}

STUB_EXPORT int media_player_close(int play_handle)
{
    (void)play_handle;
    return -1;
}

STUB_EXPORT int media_player_input_media_data(int play_handle, int stream_type, void *frame)
{
    (void)play_handle;
    (void)stream_type;
    (void)frame;
    return -1;
}

STUB_EXPORT int hw_display_get_module(const void **module, const char *inst)
{
    (void)module;
    (void)inst;
    return -1;
}
