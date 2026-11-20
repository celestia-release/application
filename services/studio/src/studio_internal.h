/*
 * studio_internal.h - shared defs for studio module sources
 *
 * Protocol: single TCP port (default 7777), stream of frames:
 *   [4B type LE][4B payload len LE][payload]
 * JSON payloads are length-delimited, no NUL. Binary payloads carry
 * their own small headers (see per-type comment below).
 */
#ifndef STUDIO_INTERNAL_H
#define STUDIO_INTERNAL_H

#include <cJSON.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "studio.h"

/*********************
 *      DEFINES
 *********************/
#define SA_LOG_TAG "STUDIO"

#define SA_DEFAULT_PORT        7777
#define SA_MAX_CLIENTS         4
#define SA_MAX_PAYLOAD         (16u * 1024u * 1024u) /* FILE_GET cap / push cap */
#define SA_EXEC_OUT_CAP        (64u * 1024u)

/* protocol frame types */
#define SA_PT_HELLO        0x01
#define SA_PT_METRICS      0x02
#define SA_PT_LOG          0x03
#define SA_PT_FRAME        0x04
#define SA_PT_SNAPSHOT     0x05
#define SA_PT_SUB          0x11 /* JSON {metrics,log,frame,fps,scale} */
#define SA_PT_EXEC         0x07
#define SA_PT_EXEC_RESULT  0x08
#define SA_PT_REBOOT       0x09
#define SA_PT_FILE_GET     0x0A /* req JSON {path}; rep [4B status][4B size][data] */
#define SA_PT_FILE_PUT     0x0B /* req [4B pathlen][4B size][path][data] */
#define SA_PT_APP_PUSH     0x0C /* req [4B jsonlen][4B datalen][manifest json][tar.gz] */
#define SA_PT_APP_RESULT   0x0D
#define SA_PT_ERR          0x0E /* JSON {error} */
#define SA_PT_PING         0x0F
#define SA_PT_PONG         0x10
#define SA_PT_INPUT        0x12 /* JSON {op:down|move|up, x, y} screen pixels */

/* install targets accepted by agent_install.c */
#define SA_INSTALL_DIR_APP  "/usr/apps"
#define SA_INSTALL_DIR_LIB  "/usr/lib"

/*********************
 *      TYPEDEFS
 **********************/
struct sa_client
{
    int fd;
    volatile bool active;
    bool want_metrics;
    bool want_log;
    bool want_frame;
    pthread_mutex_t send_lock;
    struct sa_client *next;
};

/*********************
 *   GLOBAL PROTOTYPES
 **********************/
/* log helper: console only; ipc_main's stderr lands on serial console */
void sa_log(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define AG_LOGI(...) sa_log("INFO ", __VA_ARGS__)
#define AG_LOGW(...) sa_log("WARN ", __VA_ARGS__)
#define AG_LOGE(...) sa_log("ERROR", __VA_ARGS__)

/* frame send helpers (all lock the client send_lock) */
int sa_send_json(struct sa_client *c, uint32_t type, const char *json);
int sa_send_raw(struct sa_client *c, uint32_t type, const uint8_t *data, uint32_t len);

/* broadcast to subscribers; safe from any thread; drops closed clients */
void sa_broadcast_json(uint32_t type, const char *json);
void sa_broadcast_frame(const uint8_t *jpeg, uint32_t len);

/* client registry (studio.c owns it; agent_frame.c reads it for idle skip) */
extern pthread_mutex_t s_clients_lock;
extern struct sa_client *s_clients;

/* metrics.c lives inside studio.c: called at 1 Hz */
void sa_metrics_tick(void);

/* agent_frame.c */
int sa_frame_init(int *out_w, int *out_h);       /* open fb, map, start encoder thread */
int sa_frame_probe(int *out_w, int *out_h);      /* query current fb geometry only */
void sa_frame_set_params(int fps, int scale);    /* fps 0..15, scale 1|2|4 */
void sa_frame_get_params(int *fps, int *scale);
/* encode one frame now (used by SNAPSHOT); returns malloc'd jpeg */
int sa_frame_grab(uint8_t **jpeg, uint32_t *len);

/* agent_install.c: handle APP_PUSH payload; fills result JSON (cJSON) */
cJSON *sa_app_install(const char *manifest_json, const uint8_t *tgz, uint32_t tlen);

/* agent_input.c: inject one touch sample in LCD pixels. down=1 pressed, 0 released. */
int sa_input_touch(int x, int y, int down, int sw, int sh);

#endif /* STUDIO_INTERNAL_H */
