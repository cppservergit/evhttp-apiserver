#define HTTP_CLIENT_CHUNK_SIZE 4096
#include "http_client.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <event2/http.h>
#include "thread_error.h"

static pthread_key_t g_curl_tls_key;
static pthread_once_t g_curl_tls_once = PTHREAD_ONCE_INIT;

struct memory_struct {
    char* memory;
    size_t size;
};

static size_t write_memory_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    struct memory_struct* mem = (struct memory_struct*)userp;

    char* ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        free(mem->memory);
        mem->memory = nullptr;
        return 0; // Out of memory
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

static void curl_thread_destructor(void* val) {
    CURL* curl = val;
    if (curl) curl_easy_cleanup(curl);
}

static void curl_init_tls_key(void) {
    pthread_key_create(&g_curl_tls_key, curl_thread_destructor);
}

static void apply_curl_defaults(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_cb);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);        // 10 second absolute timeout
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);  // 5 second connect timeout
    
    // Explicitly pin secure TLS verification
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    // Require TLS 1.2 or higher
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_DEFAULT);
    
    // Defend against SSRF by strictly restricting allowed schemes
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
    
    // Disable HTTP redirects entirely to prevent redirect-based SSRF
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
}

static CURL* get_thread_curl(void) {
    pthread_once(&g_curl_tls_once, curl_init_tls_key);
    CURL* curl = pthread_getspecific(g_curl_tls_key);
    if (!curl) {
        curl = curl_easy_init();
        if (curl) {
            apply_curl_defaults(curl);
            pthread_setspecific(g_curl_tls_key, curl);
        }
    }
    return curl;
}

static CURL* setup_curl_request(const char* url, const char* body, const char** headers, int num_headers, struct curl_slist** out_headers, struct memory_struct* chunk) {
    CURL* curl = get_thread_curl();
    if (!curl) return nullptr;

    curl_easy_reset(curl);
    apply_curl_defaults(curl);

    chunk->memory = malloc(HTTP_CLIENT_CHUNK_SIZE);
    if (!chunk->memory) {
        set_thread_error(TL_ERR_ERROR, "Out of memory allocating initial chunk memory in do_http_request");
        return nullptr;
    }
    chunk->size = 0;
    chunk->memory[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)chunk);
    
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    
    struct curl_slist* chunk_headers = nullptr;
    for (int i = 0; i < num_headers; ++i) {
        chunk_headers = curl_slist_append(chunk_headers, headers[i]);
    }
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk_headers);
    *out_headers = chunk_headers;
    return curl;
}

static struct json_object* parse_curl_response(CURL* curl, CURLcode res, const char* url, struct memory_struct* chunk, long* out_http_code) {
    struct json_object* json_response = nullptr;
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (out_http_code) *out_http_code = http_code;
        
        if (http_code >= 400) {
            set_thread_error(TL_ERR_ERROR, "HTTP %ld from %s | Response: %s", http_code, url, chunk->memory ? chunk->memory : "<empty>");
        }
        
        if (chunk->size > 0) {
            json_response = json_tokener_parse(chunk->memory);
        }
    } else {
        set_thread_error(TL_ERR_ERROR, "libcurl network failure: %s | URL: %s", curl_easy_strerror(res), url);
        if (out_http_code) *out_http_code = HTTP_SERVUNAVAIL;
    }
    return json_response;
}

static struct json_object* do_http_request(const char* base_url, const char* uri, const char* body, const char** headers, int num_headers, long* out_http_code) {
    char url[1024];
    int written = 0;
    if (base_url && uri) {
        written = snprintf(url, sizeof(url), "%s%s", base_url, uri);
    } else if (base_url) {
        written = snprintf(url, sizeof(url), "%s", base_url);
    } else if (uri) {
        written = snprintf(url, sizeof(url), "%s", uri);
    } else {
        return nullptr;
    }
    
    if (written < 0 || written >= (int)sizeof(url)) {
        set_thread_error(TL_ERR_ERROR, "URL truncation error");
        if (out_http_code) *out_http_code = HTTP_INTERNAL;
        return nullptr;
    }

    struct memory_struct chunk;
    struct curl_slist* chunk_headers = nullptr;
    CURL* curl = setup_curl_request(url, body, headers, num_headers, &chunk_headers, &chunk);
    if (!curl) return nullptr;

    CURLcode res = curl_easy_perform(curl);
    
    if (chunk_headers) {
        curl_slist_free_all(chunk_headers);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, (struct curl_slist*)nullptr);
    }

    struct json_object* json_response = parse_curl_response(curl, res, url, &chunk, out_http_code);
    free(chunk.memory);
    return json_response;
}

struct json_object* http_client_get_json(const char* base_url, const char* uri, const char** headers, int num_headers, long* out_http_code) {
    return do_http_request(base_url, uri, nullptr, headers, num_headers, out_http_code);
}

struct json_object* http_client_post_json(const char* base_url, const char* uri, const char* body, const char** headers, int num_headers, long* out_http_code) {
    return do_http_request(base_url, uri, body, headers, num_headers, out_http_code);
}

void http_client_init_thread(void) {
    get_thread_curl(); // Eagerly initialize libcurl handle for the calling thread
}

void http_client_cleanup_thread(void) {
    // Curl handles are automatically destroyed by the pthread_key destructor (curl_thread_destructor) when the thread exits.
}
