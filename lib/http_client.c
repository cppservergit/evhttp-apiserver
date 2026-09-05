#include <apiserver/http_client.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdckdint.h>
#include <event2/http.h>
#include <apiserver/thread_error.h>
#include <apiserver/config.h>
#include <apiserver/logger.h>
constexpr int HTTP_CLIENT_CHUNK_SIZE = 4096;
static pthread_key_t g_curl_tls_key;
static pthread_once_t g_curl_tls_once = PTHREAD_ONCE_INIT;

struct memory_struct {
    char* memory;
    size_t size;
    size_t capacity;
};

static inline void cleanup_memory_struct(struct memory_struct* mem) {
    if (mem->memory) free(mem->memory);
}

static inline void cleanup_curl_slist(struct curl_slist** slist) {
    if (*slist) curl_slist_free_all(*slist);
}


static size_t write_memory_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    if (nmemb && size > SIZE_MAX / nmemb) return 0;
    size_t realsize = size * nmemb;
    struct memory_struct* mem = (struct memory_struct*)userp;

    size_t new_size;
    if (ckd_add(&new_size, mem->size, realsize)) {
        return 0; // Overflow
    }

    if (new_size > config_get_max_payload_size()) {
        return 0; // Abort transfer, exceeds max size
    }

    size_t req_cap;
    if (ckd_add(&req_cap, new_size, 1)) {
        return 0; // Overflow
    }

    if (req_cap > mem->capacity) {
        size_t new_cap = mem->capacity;
        if (ckd_mul(&new_cap, new_cap, 2)) new_cap = req_cap;
        if (new_cap < req_cap) {
            new_cap = req_cap;
        }
        char* ptr = realloc(mem->memory, new_cap);
        if (!ptr) {
            set_thread_error(TL_ERR_ERROR, "Out of memory in HTTP client write callback");
            free(mem->memory);
            mem->memory = nullptr;
            mem->size = 0;
            mem->capacity = 0;
            return 0; // Out of memory
        }
        mem->memory = ptr;
        mem->capacity = new_cap;
    }
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size = new_size;
    mem->memory[mem->size] = 0;

    return realsize;
}

static void curl_thread_destructor(void* val) {
    CURL* curl = val;
    if (curl) curl_easy_cleanup(curl);
}

static void curl_init_tls_key(void) {
    if (pthread_key_create(&g_curl_tls_key, curl_thread_destructor) != 0) {
        LOG_FATAL("Failed to create pthread key for CURL connections");
        abort();
    }
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
    
    // Set custom User-Agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "evhttp-apiserver/1.0");
    
    // Require TLS 1.2 or higher
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_DEFAULT));
    
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
    chunk->capacity = HTTP_CLIENT_CHUNK_SIZE;
    chunk->memory[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)chunk);
    
    if (body) {
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    
    struct curl_slist* chunk_headers = nullptr;
    for (int i = 0; i < num_headers; ++i) {
        struct curl_slist* temp = curl_slist_append(chunk_headers, headers[i]);
        if (!temp) {
            set_thread_error(TL_ERR_ERROR, "Out of memory allocating curl headers");
            *out_headers = chunk_headers; // So RAII can free the ones successfully allocated
            // Note: chunk->memory was already allocated above. It will be freed automatically 
            // by the [[gnu::cleanup]] macro on the 'chunk' variable in the caller (do_http_request).
            return nullptr;
        }
        chunk_headers = temp;
    }
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk_headers);
    *out_headers = chunk_headers;
    return curl;
}


static bool build_request_url(const char* base_url, const char* uri, char* url, size_t max_len) {
    int written = 0;
    if (base_url && uri) {
        written = snprintf(url, max_len, "%s%s", base_url, uri);
    } else if (base_url) {
        written = snprintf(url, max_len, "%s", base_url);
    } else if (uri) {
        written = snprintf(url, max_len, "%s", uri);
    } else {
        return false;
    }
    return (written >= 0 && (size_t)written < max_len);
}
static bool perform_http_request(const char* base_url, const char* uri, const char* body, const char** headers, int num_headers, struct memory_struct* chunk, long* out_http_code) {
    char url[1024];
    if (!build_request_url(base_url, uri, url, sizeof(url))) {
        set_thread_error(TL_ERR_ERROR, "URL truncation error");
        if (out_http_code) *out_http_code = HTTP_INTERNAL;
        return false;
    }

    [[gnu::cleanup(cleanup_curl_slist)]] struct curl_slist* chunk_headers = nullptr;
    CURL* curl = setup_curl_request(url, body, headers, num_headers, &chunk_headers, chunk);
    if (!curl) return false;

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, (struct curl_slist*)nullptr);

    if (res != CURLE_OK) {
        set_thread_error(TL_ERR_ERROR, "libcurl network failure: %s (%s) | URL: %s", curl_easy_strerror(res), errbuf[0] ? errbuf : "no details", url);
        if (out_http_code) *out_http_code = HTTP_SERVUNAVAIL;
        return false;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (out_http_code) *out_http_code = http_code;
    
    if (http_code >= 400) {
        char trunc_body[256] = {0};
        if (chunk->memory) {
            (void)snprintf(trunc_body, sizeof(trunc_body), "%s", chunk->memory);
        }
        set_thread_error(TL_ERR_ERROR, "HTTP %ld from %s | Response: %s", http_code, url, trunc_body[0] ? trunc_body : "<empty>");
    }

    return true;
}

static struct json_object* do_http_request(const char* base_url, const char* uri, const char* body, const char** headers, int num_headers, long* out_http_code) {
    [[gnu::cleanup(cleanup_memory_struct)]] struct memory_struct chunk = {0};
    if (perform_http_request(base_url, uri, body, headers, num_headers, &chunk, out_http_code)) {
        if (chunk.size > 0 && chunk.memory) {
            return json_tokener_parse(chunk.memory);
        }
    }
    return nullptr;
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
    // Intentional no-op kept for API symmetry. 
    // Curl handles are automatically destroyed by the pthread_key destructor 
    // (curl_thread_destructor) when the thread exits.
}

char* http_client_post_raw(const char* base_url, const char* uri, const char* body, const char** headers, int num_headers, long* out_http_code) {
    [[gnu::cleanup(cleanup_memory_struct)]] struct memory_struct chunk = {0};
    if (perform_http_request(base_url, uri, body, headers, num_headers, &chunk, out_http_code)) {
        if (chunk.size > 0 && chunk.memory) {
            return strdup(chunk.memory);
        }
    }
    return nullptr;
}
