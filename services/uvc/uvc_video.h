// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/services/uvc.
 *
 * UVC video: resolution, codec, stream types. ak* types are HAL compatibility (see NAMING_CONVENTIONS).
 */
#ifndef APPLICATION_UVC_VIDEO_H
#define APPLICATION_UVC_VIDEO_H

#include "uvc_media_compat.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_GC5603           1
//#define CAMERA_OG01A1X          1

/* resolation define */
struct resolution_t {
    unsigned int width;
    unsigned int height;
    unsigned char str[20];
};

#if CAMERA_OG01A1X
typedef enum {
    DE_VIDEO_240P = 0,
    DE_VIDEO_320P,
    DE_VIDEO_VGA,
    DE_VIDEO_480P,
    DE_VIDEO_720P,
    DE_VIDEO_1024P,
}ch_resolutions;
#else
typedef enum {
//    DE_VIDEO_240P = 0,
//    DE_VIDEO_320P,
//    DE_VIDEO_VGA,
//    DE_VIDEO_480P,
//    DE_VIDEO_720P,
    DE_VIDEO_1080P,
//    DE_VIDEO_1440P,
}ch_resolutions;
#endif

typedef enum akBITRATE_CTRL_MODE_E {
    BR_MODE_CBR,
    BR_MODE_VBR,
    BR_MODE_CAPPED_VBR,
    BR_MODE_CONST_QP,
    BR_MODE_LOW_LATENCY,
    BR_MODE_AVBR,
    BR_MODE_TYPE_TOTAL
}BITRATE_CTRL_MODE_E;

/* video encode frame define */
typedef enum akVIDEO_FRAME_TYPE_E {
    FRAME_TYPE_P,
    FRAME_TYPE_I,
    FRAME_TYPE_PI,
}VIDEO_FRAME_TYPE_E;

typedef struct akVIDEO_STREAM_S {
    uint8_t *pu8Data;	//stream data
    uint32_t u32Len; 		//stream len in bytes
    uint64_t u64Ts;	//timestamp(ms)
    uint64_t u64SeqNo;	//current stream sequence no according to frame
    VIDEO_FRAME_TYPE_E enFrameType;	//I or P frame
}VIDEO_STREAM_S;

struct dev_ch_t {
     int main_ch;
     int sub_ch;
};


int video_uvc_init(fw_video_enc_param *pvenc, AA_STREAM_CB get_frame_fun, AA_RESOLUTION_CB set_resolution_fun);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_UVC_VIDEO_H */
