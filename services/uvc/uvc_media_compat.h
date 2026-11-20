/*
 * UVC layer compatibility: legacy fw_* types used by uvc_video/uvc_common.
 * uvc_server includes multimedia.h and copies between media_* and these.
 */
#ifndef APPLICATION_SERVICES_UVC_MEDIA_COMPAT_H
#define APPLICATION_SERVICES_UVC_MEDIA_COMPAT_H

/* Legacy frame type used by uvc_video callback and uvc_save_frame_to_file */
typedef struct {
    int code_type;
    int frame_type;
    unsigned int uiDataLen;
    unsigned int seq_no;
    unsigned long long u64Time;
    int iWidth;
    int iHeight;
    unsigned char *pData;
} fw_frame_info;

/* Legacy video enc param used by video_uvc_init and uvc_video_config_encode_param */
#define UVC_CODEC_TYPE_H264  0
#define UVC_CODEC_TYPE_MJPEG 1
#define UVC_CODEC_TYPE_H265  2
typedef struct {
    int code_type;
    int width;
    int height;
    int framerate;
    int bitrate;
    int gop;
    int bitrate_mode;
} fw_video_enc_param;

/* Callback type used by video_uvc_init */
typedef int (*AA_STREAM_CB)(int nHandle, fw_frame_info *pframe, void *pDataCbUser);
typedef int (*AA_RESOLUTION_CB)(int nHandle, int width, int height);

#endif /* APPLICATION_SERVICES_UVC_MEDIA_COMPAT_H */
