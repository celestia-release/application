// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_CALL_H
#define APPLICATION_TABLET_CALL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tablet_common.h"
#include "notification.h"

#ifdef CONFIG_APP_TABLET_IPC_CAMERA
lv_result_t tablet_call_create(lv_obj_t *main, int call_mode);
int ipc_call_network_ready(void);
#endif

/* Unchanged callback declarations */
int tablet_call_msg_callback(int enable, void *pUser);
int tablet_call_connect_state_callback(message_t *pMsg, void *pUser);
int tablet_call_get_state(void);
int tablet_call_get_net_connect_state(void);

#ifdef __cplusplus
}
#endif

#endif
