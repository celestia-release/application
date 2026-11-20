/*********************
 *      INCLUDES
 *********************/
#include "weather_service.h"
#include "app_globle_config.h"

#if CONFIG_NETWORK_SUPPORTED
#include "log.h"
#include "system_error.h"
#include <cJSON.h>
#include <curl.h>
#include <easy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*********************
 *      DEFINES
 *********************/


/* Define LOG_TAG after all includes (headers should not define LOG_TAG) */
#define LOG_TAG "WEATHER_SERVICE"
#define CONFIG_PRINTF_WEATHER_INFO          0

#define WEATHER_TO_IMAGE_NUM            39  /* Number of elements in weather_to_image_t array */
#define WEATHER_CONFIG_PATH             "/etc/config/weather.json"
#define WEATHER_CACHE_PATH              "/etc/config/weather.conf"
#define WEATHER_DEFAULT_IP_GEO_URL \
    "http://ip-api.com/json/?fields=status,message,city,country&lang=zh-CN"
#define WEATHER_DEFAULT_API_TMPL \
    "http://api.seniverse.com/v3/weather/now.json?key={key}&location={location}&language=zh-Hans&unit=c"
#define WEATHER_DEFAULT_API_KEY         "YOUR_WEATHER_API_KEY"
#define WEATHER_DEFAULT_FALLBACK_LOC    "guangzhou"

/**********************
 *      TYPEDEFS
 **********************/

/* Buffer format for CURL response body */
typedef struct
{
    char *memory;
    size_t size;
} curl_memory_t;

/* Weather data structure reported to GUI */
typedef struct
{
    char text[100];      /* Weather description */
    char code[5];        /* Weather code */
    char temperature[5]; /* Temperature string */
} weather_t;

typedef struct
{
    char weather_text[30]; /* Weather text, max ~14 bytes for longest entry, 30 allocated */
    char code[5];          /* Weather code "0"~"99" (matches API) */
    char image_dir[20];    /* Icon path e.g. "weather/xx_2x", 20 bytes */
} weather_to_image_t;

/* Weather info for UI display */
typedef struct weather_ui_display
{
    char image_dir[20];           /* Weather icon path */
    char weather_lable_text[100]; /* Weather + temperature string */
} weather_ui_display_t;

typedef struct {
    char ip_geo_url[256];
    char weather_api_url_template[512];
    char weather_api_key[128];
    char fallback_location[64];
} weather_runtime_cfg_t;

typedef struct {
    const char *city_zh;
    const char *location;
} weather_city_map_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static size_t WriteMemoryCallback(void *ptr, size_t size, size_t nmemb, void *data);
static int curl_fetch(const char *url, char **out_body, size_t *out_size);
static int curl_get(const char *url, const char *savefilename);
static void weather_load_config(weather_runtime_cfg_t *cfg);
static const char *weather_city_to_location(const char *city_zh, const char *fallback);
static int weather_build_api_url(char *out, size_t out_len, const char *tmpl,
                                 const char *key, const char *location);
static int weather_resolve_location(const weather_runtime_cfg_t *cfg, char *loc_out, size_t loc_len);
static int weather_parse_and_update_ui(update_weather_callback callback);
static int weather_data_to_ui(void);
static void printf_weather_info(const char *name, const char *info);

/**********************
 *  STATIC VARIABLES
 **********************/

static weather_t g_weather_data;
static weather_ui_display_t g_weather_display;

/* Chinese city name (ip-api lang=zh-CN) → seniverse location slug */
static const weather_city_map_t g_city_map[] = {
    {"广州", "guangzhou"},
    {"广州市", "guangzhou"},
    {"北京", "beijing"},
    {"北京市", "beijing"},
    {"上海", "shanghai"},
    {"上海市", "shanghai"},
    {"深圳", "shenzhen"},
    {"深圳市", "shenzhen"},
    {"杭州", "hangzhou"},
    {"杭州市", "hangzhou"},
    {"成都", "chengdu"},
    {"成都市", "chengdu"},
    {"重庆", "chongqing"},
    {"重庆市", "chongqing"},
    {"武汉", "wuhan"},
    {"武汉市", "wuhan"},
    {"西安", "xian"},
    {"西安市", "xian"},
    {"南京", "nanjing"},
    {"南京市", "nanjing"},
    {"天津", "tianjin"},
    {"天津市", "tianjin"},
    {"苏州", "suzhou"},
    {"苏州市", "suzhou"},
    {"长沙", "changsha"},
    {"长沙市", "changsha"},
    {"郑州", "zhengzhou"},
    {"郑州市", "zhengzhou"},
    {"东莞", "dongguan"},
    {"东莞市", "dongguan"},
    {"佛山", "foshan"},
    {"佛山市", "foshan"},
    {"厦门", "xiamen"},
    {"厦门市", "xiamen"},
    {"青岛", "qingdao"},
    {"青岛市", "qingdao"},
    {"大连", "dalian"},
    {"大连市", "dalian"},
    {"宁波", "ningbo"},
    {"宁波市", "ningbo"},
    {"珠海", "zhuhai"},
    {"珠海市", "zhuhai"},
    {"中山", "zhongshan"},
    {"中山市", "zhongshan"},
    {"惠州", "huizhou"},
    {"惠州市", "huizhou"},
    {"合肥", "hefei"},
    {"合肥市", "hefei"},
    {"福州", "fuzhou"},
    {"福州市", "fuzhou"},
    {"济南", "jinan"},
    {"济南市", "jinan"},
    {"昆明", "kunming"},
    {"昆明市", "kunming"},
    {"沈阳", "shenyang"},
    {"沈阳市", "shenyang"},
    {"香港", "hongkong"},
    {"澳门", "macau"},
    {"台北", "taipei"},
    {"台北市", "taipei"},
};

static weather_to_image_t weather_to_image_array[WEATHER_TO_IMAGE_NUM] = {
    // text          code image_dir
#if 0
    {"Sunny",       "0",    "weather/0_2x.png"},    /* sunny */
    {"Sunny",       "1",    "weather/1_2x.png"},    /* clear night */
    {"Fair",        "2",    "weather/2_2x.png"},    /* fair day */
    {"Fair",        "3",    "weather/3_2x.png"},    /* fair night */
    {"Cloudy",      "4",    "weather/4_2x.png"},    /* cloudy */
    {"Cloudy",      "5",    "weather/5_2x.png"},    /* partly cloudy day */
    {"Cloudy",      "6",    "weather/6_2x.png"},   /* partly cloudy night */
    {"Cloudy",      "7",    "weather/7_2x.png"},    /* mostly cloudy day */
    {"Cloudy",      "8",    "weather/8_2x.png"},    /* mostly cloudy night */
    {"Overcast",    "9",    "weather/9_2x.png"},    /* overcast */
    {"Shower",      "10",   "weather/10_2x.png"},   /* shower */
    {"Shower",      "11",   "weather/11_2x.png"},   /* thundershower */
    {"Shower",      "12",   "weather/12_2x.png"},   /* thundershower with hail */
    {"Rain",        "13",   "weather/13_2x.png"},   /* light rain */
    {"Rain",        "14",   "weather/14_2x.png"},   /* moderate rain */
    {"Rain",        "15",   "weather/15_2x.png"},   /* heavy rain */
    {"Rain",        "16",   "weather/16_2x.png"},   /* storm */
    {"Rain",        "17",   "weather/17_2x.png"},   /* heavy storm */
    {"Rain",        "18",   "weather/18_2x.png"},   /* severe storm */
    {"Rain",        "19",   "weather/19_2x.png"},   /* ice rain */
    {"Sleet",       "20",   "weather/20_2x.png"},   /* sleet */
    {"Snow",        "21",   "weather/21_2x.png"},   /* snow flurry */
    {"Snow",        "22",   "weather/22_2x.png"},   /* light snow */
    {"Snow",        "23",   "weather/23_2x.png"},   /* moderate snow */
    {"Snow",        "24",   "weather/24_2x.png"},   /* heavy snow */
    {"Snow",        "25",   "weather/25_2x.png"},   /* snowstorm */
    {"Dust",        "26",   "weather/26_2x.png"},   /* dust */
    {"Sand",        "27",   "weather/27_2x.png"},   /* sand */
    {"Duststorm",   "28",   "weather/28_2x.png"},   /* duststorm */
    {"Duststorm",   "29",   "weather/29_2x.png"},   /* sandstorm */
    {"Foggy",       "30",   "weather/30_2x.png"},   /* fog */
    {"Haze",        "31",   "weather/31_2x.png"},   /* haze */
    {"Windy",       "32",   "weather/32_2x.png"},   /* wind */
    {"Blustery",    "33",   "weather/33_2x.png"},   /* blustery */
    {"Hurricane",   "34",   "weather/34_2x.png"},   /* hurricane */
    {"Storm",       "35",   "weather/35_2x.png"},   /* tropical storm */
    {"Tornado",     "36",   "weather/36_2x.png"},   /* tornado */
    {"Cold",        "37",   "weather/37_2x.png"},   /* cold */
    {"Hot",         "38",   "weather/38_2x.png"},   /* hot */
#else

    {"晴",          "0",    "weather/0_2x.png"},    // 白天晴 sunny
    {"晴",          "1",    "weather/1_2x.png"},    // 夜晚晴 clear
    {"晴",          "2",    "weather/2_2x.png"},    // 国外城市白天晴
    {"晴",          "3",    "weather/3_2x.png"},    // 国外城市夜晚晴
    {"多云",        "4",    "weather/4_2x.png"},    // 多云 cloudy
    {"多云",        "5",    "weather/5_2x.png"},    // 白天晴间多云 partly cloudy
    {"多云",        "6",    "weather/6_2x.png"},    // 夜晚晴间多云 partly cloudy
    {"多云",        "7",    "weather/7_2x.png"},    // 白天大部多云 mostly cloudy
    {"多云",        "8",    "weather/8_2x.png"},    // 夜晚大部多云 mostly cloudy
    {"阴天",        "9",    "weather/9_2x.png"},    // 阴天 overcast
    {"阵雨",        "10",   "weather/10_2x.png"},   // 阵雨 shower
    {"雷阵雨",      "11",   "weather/11_2x.png"},   // 雷阵雨 thundershower
    {"雷阵雨",      "12",   "weather/12_2x.png"},   // 雷阵雨伴有冰雹 thundershower with hail
    {"小雨",        "13",   "weather/13_2x.png"},   // 小雨 light rain
    {"中雨",        "14",   "weather/14_2x.png"},   // 中雨 moderate rain
    {"大雨 ",       "15",   "weather/15_2x.png"},   // 大雨 heavy rain
    {"暴雨",        "16",   "weather/16_2x.png"},   // 暴雨 storm
    {"大暴雨",      "17",   "weather/17_2x.png"},   // 大暴雨 heavy storm
    {"特大暴雨",    "18",   "weather/18_2x.png"},   // 特大暴雨 severe storm
    {"冻雨",        "19",   "weather/19_2x.png"},   // 冻雨 ice rain
    {"雨夹雪",      "20",   "weather/20_2x.png"},   // 雨夹雪 sleet
    {"阵雪",        "21",   "weather/21_2x.png"},   // 阵雪 snow flurry
    {"小雪",        "22",   "weather/22_2x.png"},   // 小雪 light snow
    {"中雪",        "23",   "weather/23_2x.png"},   // 中雪 moderate snow
    {"大雪",        "24",   "weather/24_2x.png"},   // 大雪 heavy snow
    {"暴雪",        "25",   "weather/25_2x.png"},   // 暴雪 snowstorm
    {"浮尘",        "26",   "weather/26_2x.png"},   // 浮尘 dust
    {"杨沙 ",       "27",   "weather/27_2x.png"},   // 杨沙 Sand
    {"沙尘暴",      "28",   "weather/28_2x.png"},   // 沙尘暴 Duststorm
    {"强沙尘暴",    "29",   "weather/29_2x.png"},   // 强沙尘暴 sandstorm
    {"雾",          "30",   "weather/30_2x.png"},   // 雾 Foggy
    {"霾",          "31",   "weather/31_2x.png"},   // 霾 Haze
    {"微风",        "32",   "weather/32_2x.png"},   // 风 Windy
    {"大风",        "33",   "weather/33_2x.png"},   // 大风 Blustery
    {"飓风",        "34",   "weather/34_2x.png"},   // 飓风 Hurricane
    {"热带风暴",    "35",   "weather/35_2x.png"},   // 热带风暴 Tropical Storm
    {"龙卷风",      "36",   "weather/36_2x.png"},   // 龙卷风 Tornado
    {"寒冷",        "37",   "weather/37_2x.png"},   // 冷 Cold
    {"热",          "38",   "weather/38_2x.png"},   // 热 Hot
#endif
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

int app_weather_update_request(update_weather_callback callback)
{
    weather_runtime_cfg_t cfg;
    char location[64];
    char weather_url[768];
    int ret;

    weather_load_config(&cfg);

    if (weather_resolve_location(&cfg, location, sizeof(location)) != SYSTEM_SUCCESS) {
        snprintf(location, sizeof(location), "%s", cfg.fallback_location);
    }

    if (weather_build_api_url(weather_url, sizeof(weather_url),
                              cfg.weather_api_url_template,
                              cfg.weather_api_key, location) != SYSTEM_SUCCESS) {
        log_error("build weather url failed");
        return SYSTEM_FAILED;
    }

    log_warn("weather request location=%s", location);

    ret = curl_get(weather_url, WEATHER_CACHE_PATH);
    if (ret != SYSTEM_SUCCESS) {
        log_error("curl weather api failed");
        return SYSTEM_FAILED;
    }

    return weather_parse_and_update_ui(callback);
}

/**
 * @brief Load /etc/config/weather.json; missing fields keep built-in defaults.
 */
static void weather_load_config(weather_runtime_cfg_t *cfg)
{
    char content[8 * 1024];
    FILE *fp;
    cJSON *root;
    cJSON *item;
    size_t n;

    snprintf(cfg->ip_geo_url, sizeof(cfg->ip_geo_url), "%s", WEATHER_DEFAULT_IP_GEO_URL);
    snprintf(cfg->weather_api_url_template, sizeof(cfg->weather_api_url_template),
             "%s", WEATHER_DEFAULT_API_TMPL);
    snprintf(cfg->weather_api_key, sizeof(cfg->weather_api_key), "%s", WEATHER_DEFAULT_API_KEY);
    snprintf(cfg->fallback_location, sizeof(cfg->fallback_location), "%s",
             WEATHER_DEFAULT_FALLBACK_LOC);

    fp = fopen(WEATHER_CONFIG_PATH, "rb");
    if (!fp) {
        log_warn("open %s failed, use defaults", WEATHER_CONFIG_PATH);
        return;
    }

    n = fread(content, 1, sizeof(content) - 1, fp);
    fclose(fp);
    content[n] = '\0';
    if (n == 0) {
        return;
    }

    root = cJSON_Parse(content);
    if (!root) {
        log_warn("parse %s failed, use defaults", WEATHER_CONFIG_PATH);
        return;
    }

    item = cJSON_GetObjectItem(root, "ip_geo_url");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        snprintf(cfg->ip_geo_url, sizeof(cfg->ip_geo_url), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "weather_api_url_template");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        snprintf(cfg->weather_api_url_template, sizeof(cfg->weather_api_url_template),
                 "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "weather_api_key");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        snprintf(cfg->weather_api_key, sizeof(cfg->weather_api_key), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "fallback_location");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        snprintf(cfg->fallback_location, sizeof(cfg->fallback_location), "%s",
                 item->valuestring);
    }

    cJSON_Delete(root);
}

static const char *weather_city_to_location(const char *city_zh, const char *fallback)
{
    size_t i;
    char trimmed[64];
    size_t len;

    if (!city_zh || city_zh[0] == '\0') {
        return fallback;
    }

    for (i = 0; i < sizeof(g_city_map) / sizeof(g_city_map[0]); i++) {
        if (strcmp(city_zh, g_city_map[i].city_zh) == 0) {
            return g_city_map[i].location;
        }
    }

    /* Try strip trailing "市" (UTF-8 E5 B8 82) when present */
    len = strlen(city_zh);
    if (len >= 3 && len < sizeof(trimmed) &&
        (unsigned char)city_zh[len - 3] == 0xE5 &&
        (unsigned char)city_zh[len - 2] == 0xB8 &&
        (unsigned char)city_zh[len - 1] == 0x82) {
        memcpy(trimmed, city_zh, len - 3);
        trimmed[len - 3] = '\0';
        for (i = 0; i < sizeof(g_city_map) / sizeof(g_city_map[0]); i++) {
            if (strcmp(trimmed, g_city_map[i].city_zh) == 0) {
                return g_city_map[i].location;
            }
        }
    }

    return fallback;
}

static int weather_build_api_url(char *out, size_t out_len, const char *tmpl,
                                 const char *key, const char *location)
{
    size_t oi = 0;
    const char *p;

    if (!out || out_len == 0 || !tmpl || !key || !location) {
        return SYSTEM_FAILED;
    }

    p = tmpl;
    while (*p) {
        if (strncmp(p, "{key}", 5) == 0) {
            size_t n = strlen(key);
            if (oi + n >= out_len) {
                return SYSTEM_FAILED;
            }
            memcpy(out + oi, key, n);
            oi += n;
            p += 5;
        } else if (strncmp(p, "{location}", 10) == 0) {
            size_t n = strlen(location);
            if (oi + n >= out_len) {
                return SYSTEM_FAILED;
            }
            memcpy(out + oi, location, n);
            oi += n;
            p += 10;
        } else {
            if (oi + 1 >= out_len) {
                return SYSTEM_FAILED;
            }
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
    return SYSTEM_SUCCESS;
}

/**
 * @brief Resolve seniverse location via IP geo; never persist city. On any
 *        geo failure, fill fallback_location.
 */
static int weather_resolve_location(const weather_runtime_cfg_t *cfg, char *loc_out, size_t loc_len)
{
    char *body = NULL;
    size_t body_len = 0;
    cJSON *root = NULL;
    cJSON *status;
    cJSON *city;
    const char *slug;
    int ret = SYSTEM_FAILED;

    if (!cfg || !loc_out || loc_len == 0) {
        return SYSTEM_FAILED;
    }

    snprintf(loc_out, loc_len, "%s", cfg->fallback_location);

    if (curl_fetch(cfg->ip_geo_url, &body, &body_len) != SYSTEM_SUCCESS || !body) {
        log_warn("ip geo curl failed, use fallback=%s", cfg->fallback_location);
        goto out;
    }

    root = cJSON_Parse(body);
    if (!root) {
        log_warn("ip geo json parse failed");
        goto out;
    }

    status = cJSON_GetObjectItem(root, "status");
    if (!cJSON_IsString(status) || strcmp(status->valuestring, "success") != 0) {
        log_warn("ip geo status not success");
        goto out;
    }

    city = cJSON_GetObjectItem(root, "city");
    if (!cJSON_IsString(city) || !city->valuestring || city->valuestring[0] == '\0') {
        log_warn("ip geo city missing");
        goto out;
    }

    slug = weather_city_to_location(city->valuestring, cfg->fallback_location);
    snprintf(loc_out, loc_len, "%s", slug);
    log_warn("ip geo city=%s location=%s", city->valuestring, loc_out);
    ret = SYSTEM_SUCCESS;

out:
    if (root) {
        cJSON_Delete(root);
    }
    free(body);
    return ret;
}

/**
 * @brief Parse cached seniverse JSON and update UI via callback on success only.
 */
static int weather_parse_and_update_ui(update_weather_callback callback)
{
    char content[64 * 1024];
    cJSON *root = NULL;
    FILE *fp;
    size_t n;
    int ok = 0;

    fp = fopen(WEATHER_CACHE_PATH, "rb");
    if (!fp) {
        log_error("failed to open %s", WEATHER_CACHE_PATH);
        return SYSTEM_FAILED;
    }

    n = fread(content, 1, sizeof(content) - 1, fp);
    fclose(fp);
    content[n] = '\0';
    if (n == 0) {
        return SYSTEM_FAILED;
    }

    root = cJSON_Parse(content);
    if (!root) {
        log_error("cJSON_Parse weather.conf failed");
        return SYSTEM_FAILED;
    }

    {
        cJSON *results = cJSON_GetObjectItem(root, "results");
        cJSON *array;
        cJSON *p_obj_location;
        cJSON *p_obj_now;
        cJSON *on;

        if (!results || results->type != cJSON_Array) {
            goto exit;
        }

        array = cJSON_GetArrayItem(results, 0);
        if (!array) {
            goto exit;
        }

        p_obj_location = cJSON_GetObjectItem(array, "location");
        p_obj_now = cJSON_GetObjectItem(array, "now");
        if (!p_obj_location || !p_obj_now) {
            goto exit;
        }

        on = cJSON_GetObjectItem(p_obj_location, "name");
        if (!on || !on->valuestring) {
            goto exit;
        }
        printf_weather_info("name", on->valuestring);

        memset(&g_weather_data, 0, sizeof(g_weather_data));

        on = cJSON_GetObjectItem(p_obj_now, "text");
        if (!on || !on->valuestring) {
            goto exit;
        }
        snprintf(g_weather_data.text, sizeof(g_weather_data.text), "%s", on->valuestring);
        printf_weather_info("text", g_weather_data.text);

        on = cJSON_GetObjectItem(p_obj_now, "code");
        if (!on || !on->valuestring) {
            goto exit;
        }
        snprintf(g_weather_data.code, sizeof(g_weather_data.code), "%s", on->valuestring);
        printf_weather_info("code", g_weather_data.code);

        on = cJSON_GetObjectItem(p_obj_now, "temperature");
        if (!on || !on->valuestring) {
            goto exit;
        }
        snprintf(g_weather_data.temperature, sizeof(g_weather_data.temperature),
                 "%s", on->valuestring);
        printf_weather_info("temperature", g_weather_data.temperature);

        if (weather_data_to_ui() != SYSTEM_SUCCESS) {
            goto exit;
        }

        log_warn("update weather, image: %s, text: %s",
                 g_weather_display.image_dir, g_weather_display.weather_lable_text);

        if (callback) {
            callback(g_weather_display.image_dir, g_weather_display.weather_lable_text);
        }
        ok = 1;
    }

exit:
    cJSON_Delete(root);
    return ok ? SYSTEM_SUCCESS : SYSTEM_FAILED;
}

/**
 * @brief   curl得到请求结果后的回调函数
 * @param   ptr
 * @param   size
 * @param   nmemb
 * @param   data
 * @return  
 */
static size_t WriteMemoryCallback(void *ptr, size_t size, size_t nmemb, void *data)
{
    size_t realsize = size * nmemb;
    curl_memory_t *mem = (curl_memory_t *)data;
    char *new_mem;

    new_mem = (char *)realloc(mem->memory, mem->size + realsize + 1);
    if (!new_mem) {
        return 0;
    }

    mem->memory = new_mem;
    memcpy(&(mem->memory[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

/**
 * @brief Fetch URL body into heap buffer (caller frees *out_body).
 * @return SYSTEM_SUCCESS on HTTP 200/201, SYSTEM_FAILED otherwise.
 */
static int curl_fetch(const char *url, char **out_body, size_t *out_size)
{
    CURLcode res;
    CURL *pCurl = NULL;
    curl_memory_t oDataChunk;
    long res_code = 0;
    static int g_bHttpInit = 0;
    int ret = SYSTEM_FAILED;

    if (!url || !out_body) {
        return SYSTEM_FAILED;
    }

    *out_body = NULL;
    if (out_size) {
        *out_size = 0;
    }

    if (g_bHttpInit == 0) {
        res = curl_global_init(CURL_GLOBAL_ALL);
        if (CURLE_OK != res) {
            log_error("failed to initialize global of curl");
            return SYSTEM_FAILED;
        }
        g_bHttpInit = 1;
    }

    pCurl = curl_easy_init();
    if (!pCurl) {
        log_error("failed to initialize easy of curl");
        return SYSTEM_FAILED;
    }

    oDataChunk.memory = (char *)malloc(1);
    oDataChunk.size = 0;
    if (!oDataChunk.memory) {
        curl_easy_cleanup(pCurl);
        return SYSTEM_FAILED;
    }
    oDataChunk.memory[0] = '\0';

    curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(pCurl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, &oDataChunk);
    curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(pCurl, CURLOPT_URL, url);

    res = curl_easy_perform(pCurl);
    if (res == CURLE_OK) {
        (void)curl_easy_getinfo(pCurl, CURLINFO_RESPONSE_CODE, &res_code);
    }

    if ((res == CURLE_OK) && (res_code == 200 || res_code == 201)) {
        *out_body = oDataChunk.memory;
        if (out_size) {
            *out_size = oDataChunk.size;
        }
        oDataChunk.memory = NULL;
        ret = SYSTEM_SUCCESS;
    } else {
        free(oDataChunk.memory);
        oDataChunk.memory = NULL;
        ret = SYSTEM_FAILED;
    }

    curl_easy_cleanup(pCurl);
    return ret;
}

/**
 * @brief   调用curl从网络获取信息并写入本地文件
 * @param   url
 * @param   savefilename
 * @return  SYSTEM_SUCCESS / SYSTEM_FAILED
 */
static int curl_get(const char *url, const char *savefilename)
{
    char *body = NULL;
    size_t size = 0;
    FILE *fp;

    if (curl_fetch(url, &body, &size) != SYSTEM_SUCCESS || !body) {
        free(body);
        return SYSTEM_FAILED;
    }

    fp = fopen(savefilename, "wb");
    if (!fp) {
        log_error("failed to open file: %s", savefilename);
        free(body);
        return SYSTEM_FAILED;
    }

    fwrite(body, 1, size, fp);
    fclose(fp);
    free(body);
    return SYSTEM_SUCCESS;
}

/**
 * @brief   将g_weather_data转换为g_weather_display供GUI使用
 * @return  SYSTEM_SUCCESS if code mapped; SYSTEM_FAILED otherwise (UI unchanged)
 */
static int weather_data_to_ui(void)
{
    int i;

    for (i = 0; i < WEATHER_TO_IMAGE_NUM; i++) {
#if CONFIG_PRINTF_WEATHER_INFO
        printf("i=%d code=%s weather_code=%s\r\n", i, g_weather_data.code,
               weather_to_image_array[i].code);
#endif
        if (0 == strcmp(g_weather_data.code, weather_to_image_array[i].code)) {
            break;
        }
    }

    if (i < WEATHER_TO_IMAGE_NUM) {
        memset(&g_weather_display, 0, sizeof(g_weather_display));
#if CONFIG_PRINTF_WEATHER_INFO
        printf("use image:%s\r\n", weather_to_image_array[i].image_dir);
#endif
        snprintf(g_weather_display.image_dir, sizeof(g_weather_display.image_dir),
                 "%s", weather_to_image_array[i].image_dir);
        snprintf(g_weather_display.weather_lable_text,
                 sizeof(g_weather_display.weather_lable_text),
                 "%s %s ℃", weather_to_image_array[i].weather_text,
                 g_weather_data.temperature);
        return SYSTEM_SUCCESS;
    }

    log_error("weather text:%s code:%s not find", g_weather_data.text, g_weather_data.code);
    return SYSTEM_FAILED;
}

static void printf_weather_info(const char *name, const char *info)
{
#if CONFIG_PRINTF_WEATHER_INFO
    printf("%s: %s\r\n", name, info);
#endif
}

static size_t http_callback(void *context, size_t size, size_t nmemb, void *data)
{
    size_t realsize = size * nmemb;

    printf("[%s][%d] context: %s\n", __FUNCTION__, __LINE__, (char *)context);
    return realsize;
}

static int http_cmd(int argc, char *argv[])
{
    CURLcode res;

    const char *url = NULL;

    if (argc > 1 )
        url = (const char *)argv[1];

    if (NULL == url)
        url = "http://www.nmc.gov.cn/f/rest/real/59287";

    res = curl_global_init(CURL_GLOBAL_ALL);
    if(CURLE_OK != res)
    {
        printf("curl init failed \n");
        return 1;
    }

    CURL *pCurl = NULL;
    pCurl = curl_easy_init();

    if(NULL == pCurl)
    {
        printf("Init CURL failed... \n");
        return -1;
    }

    //curl_easy_setopt(pCurl, CURLOPT_DNS_SERVERS, "114.114.114.114,8.8.8.8");
    curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, 3L);                        //请求超时时长
    curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, 10L);                //连接超时时长
    curl_easy_setopt(pCurl, CURLOPT_FOLLOWLOCATION, 1L);                 //允许重定向
    curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, http_callback); //得到请求结果后的回调函数

    curl_memory_t oDataChunk; //请求结果的保存格式
    oDataChunk.memory = (char *)malloc(1);
    oDataChunk.size = 0;
    curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, &oDataChunk);

    curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L); //关闭中断信号响应
    curl_easy_setopt(pCurl, CURLOPT_URL, url);     //需要获取的URL地址

    res = curl_easy_perform(pCurl); //执行请求

    long res_code = 0;
    res = curl_easy_getinfo(pCurl, CURLINFO_RESPONSE_CODE, &res_code);

    printf("[%s][%d] CURL: res: %d, code: %ld\n", __FUNCTION__, __LINE__, res, res_code);

    //正确响应后，请请求转写成本地文件的文件
    if((res == CURLE_OK) && (res_code == 200 || res_code == 201))
    {
        printf("get infor success from curl\n");
    }
    else
    {
        printf("failed to get infor from curl\n");
        return -1;
    }

    curl_easy_cleanup(pCurl);

    curl_global_cleanup();

    return 0;
}

#ifdef AK_RTOS
SHELL_CMD_EXPORT_ALIAS(http_cmd, httpc, http command);
#endif

#endif

