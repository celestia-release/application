// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * No-op when CONFIG_APP_MAIN_DEFER_HEAVY_SO is unset (e.g. anypi_nova_dev).
 * FreeType/libpng are linked normally; tablet_entry may still call this API.
 */
#include "app_main.h"

int app_main_load_deferred_gui_libs(void)
{
    return 0;
}
