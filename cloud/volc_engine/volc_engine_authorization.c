#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <curl/curl.h>
#include "volc_engine_authorization.h"

#define MAX_BUFFER_SIZE 4096
/* mbedtls has no OpenSSL SHA256_DIGEST_LENGTH */
#ifndef SHA256_DIGEST_LENGTH
#define SHA256_DIGEST_LENGTH 32
#endif

/* SHA256 / HMAC-SHA256 via mbedtls (aligned with curl mbedtls TLS; no libcrypto). */

void hash_sha256(const char *content, unsigned char *output) {
    mbedtls_sha256((const unsigned char *)content, strlen(content), output, 0);
}

void hmac_sha256(const unsigned char *key, int key_len,
                const char *content, unsigned char *output) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (info == NULL) {
        memset(output, 0, 32);
        return;
    }
    (void)mbedtls_md_hmac(info, key, (size_t)key_len,
                          (const unsigned char *)content, strlen(content), output);
}
// 将二进制数据转换为十六进制字符串
void bin2hex(const unsigned char *bin, size_t len, char *hex) {
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + (i * 2), "%02x", bin[i]);
    }
    hex[len * 2] = '\0';
}
int hex_string_to_bytes(const char *hex_str, unsigned char *out, size_t out_len) {
    size_t len = strlen(hex_str);
    if (len % 2 != 0) {
        fprintf(stderr, "错误：十六进制字符串长度必须为偶数\n");
        return -1;
    }

    if (len / 2 > out_len) {
        fprintf(stderr, "错误：输出缓冲区太小\n");
        return -1;
    }

    for (size_t i = 0; i < len; i += 2) {
        char byte_str[3] = {0};
        byte_str[0] = hex_str[i];
        byte_str[1] = hex_str[i + 1];

        int value;
        if (sscanf(byte_str, "%2x", &value) != 1) {
            fprintf(stderr, "错误：无法解析十六进制字符 %c%c\n", hex_str[i], hex_str[i + 1]);
            return -1;
        }
        out[i / 2] = (unsigned char)value;
    }

    return 0;  /* success */
}

/* Callback to handle HTTP response body */
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    char **response = (char **)userp;
    *response = realloc(*response, realsize + 1);
    if (*response == NULL) {
        return 0;
    }
    memcpy(*response, contents, realsize);
    (*response)[realsize] = '\0';
    return realsize;
}
int request_rtc_api_start(const char *http_host, const char *http_method, 
                   const char *canonical_uri, const char *canonical_query,
                   const char *body_content, const char *AK, const char *SK,HTTPS_WRITE_CALLBACK write_cb,void *param) {
    CURL *curl;
    CURLcode res;
//    char *response = NULL;
    
    // 步骤1：创建规范请求


//    CanonicalRequest = 
//            HTTPRequestMethod + '\n' + 
//            CanonicalURI + '\n' + 
//            CanonicalQueryString + '\n' + 
//            CanonicalHeaders + '\n' + 
//            SignedHeaders + '\n' + 
//            HexEncode(Hash(RequestPayload))

    
    time_t now;
    time(&now);
    struct tm *gmt = gmtime(&now);
    char x_date[20] = {0};
    char ymd_date[20] = {0};
    
    static char time_str[20] = {0};
	time_t t= time(NULL);

//    struct tm *value = localtime(&t);
//    sprintf(x_date, "%.4d%.2d%.2dT%.2d%.2d%.2dZ",
//        (1900+value->tm_year), (value->tm_mon + 1), value->tm_mday,
//        value->tm_hour, value->tm_min, value->tm_sec);

    strftime(x_date, sizeof(x_date), "%Y%m%dT%H%M%SZ", gmt);
    strncpy(ymd_date,x_date,strlen("yyyymmdd"));

    // 1)
    char canonical_request[MAX_BUFFER_SIZE] = {0};

    /* HexEncode(Hash(RequestPayload))  */
    unsigned char body_hash[SHA256_DIGEST_LENGTH];
    hash_sha256(body_content, body_hash);
    char body_hash_hex[65] = {0};
    bin2hex(body_hash, SHA256_DIGEST_LENGTH, body_hash_hex);

    /* CanonicalHeaders  */
    //const char *content_type = "application/json";
    //char x_content_sha256[128] = {0};

    char canonical_headers[512] = {0};
    snprintf(canonical_headers, sizeof(canonical_headers),
             "host:%s\nx-content-sha256:%s\nx-date:%s\n",http_host,body_hash_hex, x_date);

    /* SignedHeaders  */     
    const char *signed_headers = "host;x-content-sha256;x-date";

    /* all  */  
    snprintf(canonical_request, sizeof(canonical_request),
             "%s\n%s\n%s\n%s\n%s\n%s",
             http_method, canonical_uri, canonical_query, 
             canonical_headers, signed_headers, body_hash_hex);



//    StringToSign =
//            Algorithm + '\n' +
//            RequestDate + '\n' +
//            CredentialScope + '\n' +
//            HexEncode(Hash(CanonicalRequest))

    // 步骤2：创建待签字符串
    char string_to_sign[MAX_BUFFER_SIZE] = {0};
    char credential_scope[50] = {0};
    
    strftime(credential_scope, sizeof(credential_scope), "%Y%m%d", gmt);

    strcat(credential_scope, "/cn-beijing/rtc/request");

    unsigned char canonical_request_hash[SHA256_DIGEST_LENGTH];
    hash_sha256(canonical_request, canonical_request_hash);
    char hashed_canonical_request[65] = {0};
    bin2hex(canonical_request_hash, SHA256_DIGEST_LENGTH, hashed_canonical_request);

    snprintf(string_to_sign, sizeof(string_to_sign),
             "HMAC-SHA256\n%s\n%s\n%s",
             x_date, credential_scope, hashed_canonical_request);




//    kSecret = Your Secret Access Key
//    kDate = HMAC(kSecret, Date)
//    kRegion = HMAC(kDate, Region)
//   kService = HMAC(kRegion, Service)
//    kSigning = HMAC(kService, "request")

    // 步骤3：构建签名
    unsigned char signature[32] = {0};
    char date_short[9] = {0};
    strncpy(date_short,credential_scope,8);
    
    // 派生签名密钥
    unsigned char k_date[32] = {0};
    unsigned char k_region[32] = {0};
    unsigned char k_server[32] = {0};
    unsigned char k_signing[32] = {0};
    
    unsigned char k_date_hex[65] = {0};
    unsigned char k_region_hex[65] = {0};
    unsigned char k_server_hex[65] = {0};
    unsigned char k_signing_hex[65] = {0};


    hmac_sha256((unsigned char*)SK, strlen(SK), date_short, k_date);
    //bin2hex(k_date, SHA256_DIGEST_LENGTH, k_date_hex);

    hmac_sha256(k_date, 32, "cn-beijing", k_region);
    //bin2hex(k_region, SHA256_DIGEST_LENGTH, k_region_hex);

    hmac_sha256(k_region, 32, "rtc", k_server);
    //bin2hex(k_server, SHA256_DIGEST_LENGTH, k_server_hex);

    hmac_sha256(k_server, 32, "request", k_signing);
    //bin2hex(k_signing, SHA256_DIGEST_LENGTH, k_signing_hex);

    // 计算签名
    hmac_sha256(k_signing, 32, string_to_sign, signature);
    char signature_hex[65] = {0};
    bin2hex(signature, 32, signature_hex);

    // 步骤4：生成Authorization头
    char authorization[512] = {0};
    snprintf(authorization, sizeof(authorization),
             "HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
             AK, credential_scope, signed_headers, signature_hex);
    // 步骤5：发起HTTP请求
    curl = curl_easy_init();
    if(curl) {
        char url[1024];
        snprintf(url, sizeof(url), "https://%s%s?%s", 
                http_host, canonical_uri, canonical_query);
    

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");


        char header_buff[4096] = {0};
        sprintf(header_buff, "Host: %s", http_host);
        headers = curl_slist_append(headers, header_buff);
        
        memset(header_buff,0,sizeof(header_buff));
        sprintf(header_buff, "X-Content-Sha256: %s", body_hash_hex);
        headers = curl_slist_append(headers, header_buff);

        memset(header_buff,0,sizeof(header_buff));
        sprintf(header_buff, "X-Date: %s", x_date);
        headers = curl_slist_append(headers, header_buff);
        //headers = curl_slist_append(headers, "X-Date: %s", x_date);

        memset(header_buff,0,sizeof(header_buff));
        sprintf(header_buff, "Authorization: %s", authorization);
        
        
        headers = curl_slist_append(headers, header_buff);
        //headers = curl_slist_append(headers, "Authorization: %s", authorization);

        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
    	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        // 启用 HTTPS 证书验证（推荐）
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, http_method);

        curl_easy_setopt(curl, CURLOPT_URL, url);
    	curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, param);

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

 //       curl_easy_setopt(curl, CURLOPT_HEADER, 0);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_content);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(body_content));

    	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);/* request timeout */
    	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L); //连接超时时长


        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            printf("rtc api: HTTP %ld\n", http_code);
            //printf("Response: %s\n", response);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        //free(response);
    }
    
    return 0;
}
int request_rtc_api_stop(const char *host, const char *http_method, 
                   const char *uri, const char *query,
                   const char *body_content, HTTPS_WRITE_CALLBACK write_cb,void *param) 
{
//	char *body_tmp = "{\"AppId\":\"%s\",\"RoomId\":\"%s\",\"TaskId\":\"%s\"}";
//	char body_content[1024] = {0};
//	sprintf(body_content,body_tmp,body,room_id,task_id);

//	const char *host = "rtc.volcengineapi.com";
//	const char *uri = "/";
//	const char *query = "Action=StoptVoiceChat&Version=2024-12-01";
//	const char *http_method = "POST";

	CURL *curl;
	CURLcode res;
	curl = curl_easy_init();
	if(curl) {
		char url[1024];
		snprintf(url, sizeof(url), "https://%s%s?%s", 
				host, uri, query);
	
		struct curl_slist *headers = NULL;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

		// 启用 HTTPS 证书验证（推荐）
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, http_method);

		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

		//curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		//curl_easy_setopt(curl, CURLOPT_WRITEDATA, param);

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

 // 	  curl_easy_setopt(curl, CURLOPT_HEADER, 0);
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_content);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(body_content));

		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);/* request timeout */
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L); //连接超时时长

		res = curl_easy_perform(curl);
		if(res != CURLE_OK) {
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
					curl_easy_strerror(res));
		} else {
			long http_code = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
			printf("HTTP Status: %ld\n", http_code);
			//printf("Response: %s\n", response);
		}
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		//free(response);
	}

	return 0;
}


int request_rtc_api_bak(const char *http_host, const char *http_method, 
                   const char *canonical_uri, const char *canonical_query,
                   const char *http_body, const char *AK, const char *SK,HTTPS_WRITE_CALLBACK write_cb) {
    CURL *curl;
    CURLcode res;
    char *response = NULL;
    
    // 步骤1：创建规范请求


//    CanonicalRequest = 
//            HTTPRequestMethod + '\n' + 
//            CanonicalURI + '\n' + 
//            CanonicalQueryString + '\n' + 
//            CanonicalHeaders + '\n' + 
//            SignedHeaders + '\n' + 
//            HexEncode(Hash(RequestPayload))

    
    time_t now;
    time(&now);
    struct tm *gmt = gmtime(&now);
    char x_date[20];
    char ymd_date[20] = {0};
    
    strftime(x_date, sizeof(x_date), "%Y%m%dT%H%M%SZ", gmt);
    strncmp(ymd_date,x_date,strlen("yyyymmdd"));

    unsigned char body_hash[SHA256_DIGEST_LENGTH];
    hash_sha256(http_body, body_hash);
    char x_content_sha256[65];
    bin2hex(body_hash, SHA256_DIGEST_LENGTH, x_content_sha256);
    
    const char *content_type = "application/json";
    char canonical_headers[512];
    snprintf(canonical_headers, sizeof(canonical_headers),
             "content-type:%s\nhost:%s\nx-content-sha256:%s\nx-date:%s\n",
             content_type, http_host, x_content_sha256, x_date);
             
    const char *signed_headers = "content-type;host;x-content-sha256;x-date";
    
    char canonical_request[MAX_BUFFER_SIZE];
    snprintf(canonical_request, sizeof(canonical_request),
             "%s\n%s\n%s\n%s\n%s\n%s",
             http_method, canonical_uri, canonical_query, 
             canonical_headers, signed_headers, x_content_sha256);
             
    unsigned char canonical_request_hash[SHA256_DIGEST_LENGTH];
    hash_sha256(canonical_request, canonical_request_hash);
    char hashed_canonical_request[65];
    bin2hex(canonical_request_hash, SHA256_DIGEST_LENGTH, hashed_canonical_request);


    // 步骤2：创建待签字符串
    char credential_scope[50];
    strftime(credential_scope, sizeof(credential_scope), "%Y%m%d", gmt);
    strcat(credential_scope, "/cn-north-1/rtc/request");
    printf("fun=%s line=%d debug here.\n",__FUNCTION__,__LINE__);
    char string_to_sign[MAX_BUFFER_SIZE];
    snprintf(string_to_sign, sizeof(string_to_sign),
             "HMAC-SHA256\n%s\n%s\n%s",
             x_date, credential_scope, hashed_canonical_request);
    
    // 步骤3：构建签名
    unsigned char signature[32];
    unsigned char signing_key[32];
    const char *date_short = credential_scope;
    credential_scope[8] = '\0'; // 截取日期部分
    
    // 派生签名密钥
    unsigned char k_date[32];
    unsigned char k_region[32];
    unsigned char k_server[32];
    unsigned char k_signing[32];

    hmac_sha256((unsigned char*)SK, strlen(SK), ymd_date, k_date);
    hmac_sha256(k_date, 32, "cn-north-1", k_region);
    hmac_sha256(k_region, 32, "rtc", k_server);
    hmac_sha256(k_server, 32, "request", k_region);

    hmac_sha256(k_region, 32, "rtc", signing_key);
    hmac_sha256(signing_key, 32, "request", signing_key);



    // 计算签名
    hmac_sha256(signing_key, 32, string_to_sign, signature);
    char signature_hex[65];
    bin2hex(signature, 32, signature_hex);
    
    // 步骤4：生成Authorization头
    char authorization[512];
    snprintf(authorization, sizeof(authorization),
             "HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
             AK, credential_scope, signed_headers, signature_hex);
    printf("fun=%s line=%d debug here.\n",__FUNCTION__,__LINE__);
    // 步骤5：发起HTTP请求
    curl = curl_easy_init();
    if(curl) {
        char url[1024];
        snprintf(url, sizeof(url), "https://%s%s?%s", 
                http_host, canonical_uri, canonical_query);
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        char header_buff[4096] = {0};
        sprintf(header_buff, "Host: %s", http_host);
        headers = curl_slist_append(headers, header_buff);

        memset(header_buff,0,sizeof(header_buff));
        sprintf(header_buff, "X-Content-Sha256: %s", x_content_sha256);
        headers = curl_slist_append(headers, header_buff);
        //headers = curl_slist_append(headers, "X-Content-Sha256: %s", x_content_sha256);

        memset(header_buff,0,sizeof(header_buff));
        sprintf(header_buff, "X-Date: %s", x_date);
        headers = curl_slist_append(headers, header_buff);
        //headers = curl_slist_append(headers, "X-Date: %s", x_date);

        memset(header_buff,0,sizeof(header_buff));
        sprintf(header_buff, "Authorization: %s", authorization);
        headers = curl_slist_append(headers, header_buff);
        //headers = curl_slist_append(headers, "Authorization: %s", authorization);
        curl_easy_setopt(curl, CURLOPT_URL, url);
    	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    	curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");
    
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, http_method);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, http_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(http_body));

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            printf("rtc api: HTTP %ld\n", http_code);
            //printf("Response: %s\n", response);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        //free(response);
    }
    
    return 0;
}