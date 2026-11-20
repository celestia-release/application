#ifndef APPLICATION_VOLC_ENGINE_AUTHORIZATION_H
#define APPLICATION_VOLC_ENGINE_AUTHORIZATION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef size_t (*HTTPS_WRITE_CALLBACK)(void *contents, size_t size, size_t nmemb, void *userp);

//int volc_engine_start_chat(const char *body,HTTPS_WRITE_CALLBACK write_cb,void *param);

//int volc_engine_stop_chat(const char *body,char *room_id,char * task_id,void *param);
int request_rtc_api_start(const char *http_host, const char *http_method, 
                   const char *canonical_uri, const char *canonical_query,
                   const char *body_content, const char *AK, const char *SK,HTTPS_WRITE_CALLBACK write_cb,void *param);

int request_rtc_api_stop(const char *host, const char *http_method,
                         const char *uri, const char *query,
                         const char *body_content, HTTPS_WRITE_CALLBACK write_cb, void *param);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_VOLC_ENGINE_AUTHORIZATION_H */

