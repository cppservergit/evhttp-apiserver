#include "http_client.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "logger.h"

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
    if (!ptr) return 0; // Out of memory

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

static void curl_thread_destructor(void* val) {
    CURL* curl = (CURL*)val;
    if (curl) curl_easy_cleanup(curl);
}

static void curl_init_tls_key(void) {
    pthread_key_create(&g_curl_tls_key, curl_thread_destructor);
}

static CURL* get_thread_curl(void) {
    pthread_once(&g_curl_tls_once, curl_init_tls_key);
    CURL* curl = (CURL*)pthread_getspecific(g_curl_tls_key);
    if (!curl) {
        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_cb);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);        // 10 second absolute timeout
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);  // 5 second connect timeout
            pthread_setspecific(g_curl_tls_key, curl);
        }
    }
    return curl;
}

static struct json_object* do_http_request(const char* url, const char* body, const char** headers, int num_headers, long* out_http_code) {
    CURL* curl = get_thread_curl();
    if (!curl) return nullptr;

    struct memory_struct chunk;
    chunk.memory = malloc(1);
    if (!chunk.memory) {
        LOG_FATAL("Out of memory allocating initial chunk memory in do_http_request");
        return nullptr;
    }
    chunk.size = 0;
    chunk.memory[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    
    struct curl_slist* chunk_headers = nullptr;
    for (int i = 0; i < num_headers; ++i) {
        chunk_headers = curl_slist_append(chunk_headers, headers[i]);
    }
    
    if (chunk_headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk_headers);
    }

    CURLcode res = curl_easy_perform(curl);
    
    if (chunk_headers) {
        curl_slist_free_all(chunk_headers);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, (struct curl_slist*)nullptr);
    }

    struct json_object* json_response = nullptr;

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (out_http_code) *out_http_code = http_code;
        
        if (http_code >= 400) {
            LOG_ERROR("HTTP %ld from %s | Response: %s", http_code, url, chunk.memory ? chunk.memory : "<empty>");
        }
        
        if (chunk.size > 0) {
            json_response = json_tokener_parse(chunk.memory);
        }
    } else {
        LOG_ERROR("libcurl network failure: %s | URL: %s", curl_easy_strerror(res), url);
        if (out_http_code) *out_http_code = 500;
    }

    free(chunk.memory);
    return json_response;
}

struct json_object* http_client_get_json(const char* url, const char** headers, int num_headers, long* out_http_code) {
    return do_http_request(url, nullptr, headers, num_headers, out_http_code);
}

struct json_object* http_client_post_json(const char* url, const char* body, const char** headers, int num_headers, long* out_http_code) {
    return do_http_request(url, body, headers, num_headers, out_http_code);
}

void http_client_init_thread(void) {
    get_thread_curl(); // Eagerly initialize libcurl handle for the calling thread
}

void http_client_cleanup_thread(void) {
    // Curl handles are automatically destroyed by the pthread_key destructor (curl_thread_destructor) when the thread exits.
}
