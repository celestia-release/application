// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/services/uvc.
 *
 * UVC video: device open/close, buffer handling, and encode parameter config.
 */
#define LOG_TAG "UVC_VIDEO"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "list.h"
#include "uvc_common.h"
#include "uvc_video.h"
#include "log.h"
#define log_info_app log_info
#define log_warn_app log_warn
#include "aas_common.h"
#include "gthread.h"
#include "aas_thread.h"
#include "system_time.h"


#define THREAD_PRIO -1
#define __STATIC__
#define PRINT(fmt, ...) printf("[video] " fmt, ##__VA_ARGS__)



#define VIDEO_MAX_COUNT         1

/*

#define ENC_TYPE_MAX            3
#define MAX_ENC_NUM             4

#define THIRD_CHN_SUPPORT       0

#define CHN_MODE                FRAME_MODE
#define DEF_FRAME_DEPTH         2
#define DEF_SLICE_DEPTH         4

#define VIDEO_DEV_MUX           2
 

#if CAMERA_OG01A1X
#define VIDEO_RESOLUTION        DE_VIDEO_1024P
#else
#define VIDEO_RESOLUTION        DE_VIDEO_1080P
#endif

decoder resolution */
/*static const struct resolution_t resolutions[] = {
#if CAMERA_OG01A1X
    {320,   240,   "DE_VIDEO_240P"},
    {480,   320,   "DE_VIDEO_320P"},
    {640,   480,   "DE_VIDEO_VGA"},
    {800,   480,   "DE_VIDEO_480P"},
    {1280,  720,  "DE_VIDEO_720P"},
    {1280,  1024, "DE_VIDEO_1024P"},
#else
//    {320,   240,   "DE_VIDEO_240P"},
//    {480,   320,   "DE_VIDEO_320P"},
//    {640,   480,   "DE_VIDEO_VGA"},
//    {800,   480,   "DE_VIDEO_480P"},
//    {1280,  720,  "DE_VIDEO_720P"},
    {1920,  1080,  "DE_VIDEO_1080P"},
//    {2560,  1440,  "DE_VIDEO_1440P"},
#endif
	{3840,	2880,  "DE_VIDEO_2880P"}
};

#define DE_VIDEO_SIZE_MAX       (ARRAY_SIZE(resolutions))
*/

struct video_param_t{
    unsigned short fps;
    unsigned int max_width;
    unsigned int max_height;
    int  dev_id;
    int  ch_id;
    char *isp_cfg;
    unsigned short goplen;
    unsigned short target_kbps;
    unsigned short max_kbps;
    BITRATE_CTRL_MODE_E br_mode;
    int venc_handle;

    struct list_head *p_head;
    char* uvc_devname;
    struct uvc_device* udev;
    int uvc_ctrl_th;
    int uvc_stream_th;
    GMutex stream_lock;
    int is_streaming;
	
	AA_STREAM_CB get_frame_fun;
    AA_RESOLUTION_CB set_resoluton_fun;
};

typedef struct stream_node{
    VIDEO_STREAM_S stream;
    struct list_head list;
}stream_list;

typedef struct
{
    int width;
    int height;
}video_resolution;

video_resolution g_tmp_videoWH = {0};

static struct video_param_t video[VIDEO_MAX_COUNT] = {
{
#if CAMERA_OG01A1X
    .max_width = 1280,
    .max_height = 1024,
    .isp_cfg = "/etc/isp_og01a1x_mipi_2lane_av200_dual.conf",
#elif CAMERA_GC5603
    .max_width = 2944,
    .max_height = 1664,
    .isp_cfg = "/etc/config/isp_gc5603_mipi_2lane_av200.conf",
#endif
    .fps = 30,
    .dev_id = 0,
    .ch_id = 0,
    .goplen = 25*2,
    .target_kbps = 1024,
    .max_kbps = 1024,
    .br_mode  = BR_MODE_VBR,
    .uvc_devname = "/dev/video8",
    .is_streaming = 0,
}
};
#define  UVC_STREAMING_MAXPKT   (512)

//static AK_S32 g_s32FrameRate = 30;      //default 30fps
//static VI_RECTANGLE_S stViRectangle = {0};
//static AK_S32 g_s32MainChnIndex = 0;    //main chn index
//static AK_S32 g_s32SubChnIndex = 1;     //sub chn index
static int g_s32DualCisMode = 0;
static int g_s32UvcDevCnt = 1;

static int change_resolution(int dev_id, int ch, int venc_handle, int width, int height)
{
    log_warn_app("set resoluton: id:%d, chn:%d, venc_handle:%d, width:%d, heigt:%d", dev_id, ch, venc_handle, width, height);
    video[dev_id].set_resoluton_fun(venc_handle, width, height);
    return 0;
}

/* end of func */

/* get the encode data from vi module */
static int video_uvc_control_th(void *arg)
{
    int ret;
    __u32 events;
    int video_dev = 0;
    //aas_thread("uvc_ctrl_th");
    struct video_param_t *video = (struct video_param_t *)arg; //venc handle
    /* for uvc */
    fd_set cfds;
    while (1) {
        /* for uvc */
        FD_ZERO(&cfds);
        /* We want both setup and data events on UVC interface.. */
        FD_SET(video->udev->uvc_fd, &cfds);
        ret = select(video->udev->uvc_fd + 1, NULL, NULL, &cfds, NULL);
        if (-1 == ret) {
            PRINT("select error %d, %s\n", errno, strerror(errno));
            continue;
        }
        if (0 == ret) {
            PRINT("select timeout\n");
            continue;
        }
        if (FD_ISSET(video->udev->uvc_fd, &cfds)) {
            events = uvc_events_process(video->udev);
            if(events == UVC_EVENT_STREAMON && video->udev->is_streaming)
            {
                //video_switch_video_dev(video_dev);
               // video_dev= !video_dev;
                PRINT("%s change resolution\n", __func__);
                video->is_streaming = 0;
                change_resolution(video->dev_id, video->ch_id,video->venc_handle,video->udev->width,video->udev->height);
                video->is_streaming = 1;
            }
            else if(events == UVC_EVENT_STREAMOFF && !video->udev->is_streaming)
            {
                video->is_streaming = 0;
            }
        }
    }
    aas_thread_delete(video->uvc_ctrl_th);
    //AK_BASE_ThreadExit();
    return 0;
}

static int video_uvc_stream_th(void *arg)
{
    int ret;
    __u32 events;
    struct video_param_t *video = (struct video_param_t *)arg; //venc handle
    fd_set dfds;
    unsigned long long lastt = 0;
    unsigned long long curt = 0;
    //AK_BASE_ThreadSetName("uvc_stream_th");
    while (1) {
        /* for uvc */
        FD_ZERO(&dfds);
        /* We want both setup and data events on UVC interface.. */
        FD_SET(video->udev->uvc_fd, &dfds);
        ret = select(video->udev->uvc_fd + 1, NULL, &dfds, NULL, NULL);
        if (-1 == ret) {
            PRINT("select error %d, %s\n", errno, strerror(errno));
            continue;
        }
        if (0 == ret) {
            PRINT("select timeout\n");
            continue;
        }
        if (FD_ISSET(video->udev->uvc_fd, &dfds)) {
            if (video->is_streaming) {
                //log_warn_app("video->is_streaming=%d",video->is_streaming);
				fw_frame_info frame = {0};
                int ret_get = video->get_frame_fun(0, &frame, NULL);
				if(ret_get == 0)
				{
                    curt = system_get_sys_run_time64();
					if(curt-lastt >= 2000)
					{
						log_warn_app("uvc_video_process one frame len=%d curt=%lld \n", frame.uiDataLen, curt);
						lastt = curt;
					}
					
					uvc_video_process(video->udev,frame.pData, frame.uiDataLen);
				}
				else
				{
					
				}
					
                //log_warn_app("uvc_video_process\n");
            }else{
                system_sleep_ms(5);
            }
        }
    }
    aas_thread_delete(video->uvc_stream_th);
    //AK_BASE_ThreadExit();
    return 0;
}
/* end of func */

/**
 * @brief Check whether the device is a valid video device.
 * @param device_path Path to the device node (e.g. /dev/video0).
 * @return 1 if valid, 0 if invalid or not a video device.
 */
int is_valid_video_device(const char *device_path)
{
    int fd;
    struct v4l2_capability cap;
    
    fd = open(device_path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    
    /* Query device capabilities */
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        close(fd);
        
        /* Accept CAPTURE, OUTPUT or STREAMING devices */
        if (cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE | 
                               V4L2_CAP_VIDEO_OUTPUT | 
                               V4L2_CAP_STREAMING)) {
            printf("Valid video device: %s\n", device_path);
            printf("  Driver: %s\n", cap.driver);
            printf("  Card: %s\n", cap.card);
            printf("  Bus info: %s\n", cap.bus_info);
            return 1;
        }
    }
    
    close(fd);
    return 0;
}


/**
 * @brief Get the first available video device.
 * @param device_path Output buffer for the device path.
 * @param path_size Size of the path buffer.
 * @return 1 on success (device found), 0 on failure (no device or buffer too small).
 */
int get_first_available_video_device(char *device_path, size_t path_size)
{
    for (int i = 0; i < 32; i++) {  /* Check /dev/video0 to video31 */
        char temp_path[64];
        snprintf(temp_path, sizeof(temp_path), "/dev/video%d", i);
        
        if (access(temp_path, F_OK) == 0) {
            if (is_valid_video_device(temp_path)) {
                printf("%s is a valid video device\n", temp_path);
                if (device_path && path_size > strlen(temp_path)) {
                    strncpy(device_path, temp_path, path_size - 1);
                    device_path[path_size - 1] = '\0';
                    return 1;  /* Device found */
                }
            }
        }
    }
    
    printf("No available video devices found\n");
    if (device_path && path_size > 0) {
        device_path[0] = '\0';  /* Ensure output string is empty */
    }
    return 0;  /* No device found */
}




int video_uvc_init(fw_video_enc_param *pvenc, AA_STREAM_CB get_frame_fun, AA_RESOLUTION_CB set_resolution_fun)
{
	//uvc_video_config_encode_param(pvenc);
	
    int ret;
    int mode = 0;//atoi(argv[1]);
    mode = !!mode;

    g_s32DualCisMode = 1;

    /* Static buffer for device name */
    static char video0_device_name[64];

    if (get_first_available_video_device(video0_device_name, sizeof(video0_device_name))) {
        /* Use the first available device */
        video[0].uvc_devname = video0_device_name;
        printf("[UVC] Using video device: %s\n", video[0].uvc_devname);
    } else {
        /* No available device found, use default */
        strncpy(video0_device_name, "/dev/video8", sizeof(video0_device_name) - 1);
        video0_device_name[sizeof(video0_device_name) - 1] = '\0';
        video[0].uvc_devname = video0_device_name;
        printf("[UVC] No available device found, using default: %s\n", video[0].uvc_devname);
    }

    for (int i=0; i<g_s32UvcDevCnt; i++)
    {
        /* open uvc device before venc thread */
        ret = uvc_open(&video[i].udev, video[i].uvc_devname);
        if(video[i].udev == NULL || ret < 0)
        {
			log_error_app("uvc open failed");
			return ret;
        }

        video[i].udev->uvc_devname = video[i].uvc_devname;
        video[i].udev->bulk = mode ;//ISO
        video[i].udev->run_standalone = 1;
        video[i].udev->io   = IO_METHOD_MMAP;
        video[i].udev->nbufs = 2; /* Ping-Pong buffers */
        video[i].udev->mult  = 0;
        video[i].udev->burst = 0;
        video[i].udev->speed = USB_SPEED_HIGH;
        video[i].udev->height =  pvenc->height;
        video[i].udev->width = pvenc->width;
		video[i].get_frame_fun = get_frame_fun;
       // video.udev->video_empty_stream = video_empty_stream;
       // video.udev->video_fill_stream = video_fill_stream;
        video[i].udev->user_dev = &video[i];
        video[i].udev->maxpkt = UVC_STREAMING_MAXPKT;
        video[i].set_resoluton_fun = set_resolution_fun;
        //PRINT( "video =%p! video.udev->user_dev=%p\n",&video[i],video[i].udev->user_dev);
        /* Init UVC events. */
        uvc_events_init(video[i].udev, g_s32DualCisMode);

        g_tmp_videoWH.width = video[i].udev->width;
        g_tmp_videoWH.height = video[i].udev->height;

        char Name[16] = { 0 };
        log_warn_app("start open stream.");
        sprintf(Name, "uvc_ctrl_th");
        aas_thread_new(&video[i].uvc_ctrl_th, video_uvc_control_th, &video[i], Name, 100*1024, THREAD_PRIO);
        sprintf(Name, "uvc_stream_th");
        aas_thread_new(&video[i].uvc_stream_th, video_uvc_stream_th, &video[i], Name, 100*1024, THREAD_PRIO);

        // ret = AK_BASE_ThreadCreate(&video[i].uvc_ctrl_th, video_uvc_control_th, &video[i], \
        //         ANYKA_THREAD_MIN_STACK_SIZE, THREAD_PRIO);
        // if(ret != AK_SUCCESS){
        //     AK_BASE_ThreadDetach(video[i].uvc_ctrl_th);
        //     log_error_app("uvc control thd create failed");
        // }

        // ret = AK_BASE_ThreadCreate(&video[i].uvc_stream_th, video_uvc_stream_th, &video[i], \
        //                         ANYKA_THREAD_MIN_STACK_SIZE, THREAD_PRIO);
        // if(ret != AK_SUCCESS){
        //     AK_BASE_ThreadDetach(video[i].uvc_stream_th);
        //     AK_BASE_ThreadDetach(video[i].uvc_ctrl_th);
        //     log_error_app("uvc stream thd create failed");
        // }
    }
	log_info_app("start uvc succes");

    return ret;
}
/* end of func */


int video_uvc_exit(void)
{
    int i;

    for (i = 0; i < g_s32UvcDevCnt; i++) {
        aas_thread_delete(video->uvc_stream_th);
        aas_thread_delete(video->uvc_ctrl_th);
        //AK_BASE_ThreadJoin(video[i].uvc_stream_th);
        //AK_BASE_ThreadJoin(video[i].uvc_ctrl_th);
    }
}

/* end of file */

