// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_SETTING_QR_SCAN_H
#define APPLICATION_TABLET_SETTING_QR_SCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tablet_common.h"

lv_result_t tablet_setting_qr_scan_create(lv_obj_t *parent);
lv_result_t tablet_setting_qr_scan_close(void);

#ifdef __cplusplus
}
#endif

#endif
