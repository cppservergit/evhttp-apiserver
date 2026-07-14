#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <libgen.h>
#include <linux/limits.h>

static pthread_rwlock_t g_config_lock = PTHREAD_RWLOCK_INITIALIZER;

static char g_odbc_conn_str[MAX_CONFIG_STR] = {0};
static char g_api_url[MAX_CONFIG_STR] = {0};
static char g_api_user[MAX_CONFIG_STR] = {0};
static char g_api_pass[MAX_CONFIG_STR] = {0};
static bool g_access_log = true;
static size_t g_num_threads = 0;
static size_t g_max_queue_size = 10000; // Default backpressure limit

static void trim_newline(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len-1] == '\n' || str[len-1] == '\r')) {
        str[len-1] = '\0';
        len--;
    }
}

static void load_env_file(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) return;

    char line[MAX_CONFIG_STR];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char* key = line;
            const char* val = eq + 1;
            setenv(key, val, 1);
        }
    }
    fclose(f);
}

void config_reload(void) {
    // Attempt to load apiserver.env file from the directory where the executable resides
    char exe_path[PATH_MAX] = {0};
    char env_path[PATH_MAX] = {0};
    if (readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) != -1) {
        char* dir = dirname(exe_path);
        snprintf(env_path, sizeof(env_path), "%s/apiserver.env", dir);
        load_env_file(env_path);
    } else {
        // Fallback to current working directory
        load_env_file("apiserver.env");
    }

    const char* env_odbc = getenv("ODBC_CONN_STR");
    const char* env_url = getenv("API_URL");
    const char* env_user = getenv("API_USER");
    const char* env_pass = getenv("API_PASS");
    const char* env_access_log = getenv("ACCESS_LOG");
    const char* env_num_threads = getenv("NUM_THREADS");
    const char* env_max_queue = getenv("MAX_QUEUE_SIZE");
    
    size_t num_threads_val = 0;
    if (env_num_threads) {
        num_threads_val = strtoul(env_num_threads, nullptr, 10);
    }

    size_t max_queue_val = 10000;
    if (env_max_queue) {
        max_queue_val = strtoul(env_max_queue, nullptr, 10);
    }
    
    bool access_log_val = true;
    if (env_access_log && (strcasecmp(env_access_log, "false") == 0 || 
                           strcasecmp(env_access_log, "0") == 0 || 
                           strcasecmp(env_access_log, "off") == 0 ||
                           strcasecmp(env_access_log, "no") == 0)) {
        access_log_val = false;
    }

    if (!env_odbc || !env_url || !env_user || !env_pass) {
        if (!env_odbc) LOG_FATAL("Missing required configuration variable: ODBC_CONN_STR");
        if (!env_url) LOG_FATAL("Missing required configuration variable: API_URL");
        if (!env_user) LOG_FATAL("Missing required configuration variable: API_USER");
        if (!env_pass) LOG_FATAL("Missing required configuration variable: API_PASS");
        return;
    }

    // Acquire write lock to safely update global configuration
    pthread_rwlock_wrlock(&g_config_lock);
    
    snprintf(g_odbc_conn_str, sizeof(g_odbc_conn_str), "%s", env_odbc);
    snprintf(g_api_url, sizeof(g_api_url), "%s", env_url);
    static bool is_first_load = true;
    if (!is_first_load && g_num_threads != num_threads_val && (g_num_threads != 0 || num_threads_val != 0)) {
        LOG_WARN("NUM_THREADS changed from %zu to %zu during hot-reload. This requires a full restart to take effect.", g_num_threads, num_threads_val);
    }

    snprintf(g_api_user, sizeof(g_api_user), "%s", env_user);
    snprintf(g_api_pass, sizeof(g_api_pass), "%s", env_pass);
    g_access_log = access_log_val;
    g_num_threads = num_threads_val;
    g_max_queue_size = max_queue_val;
    is_first_load = false;
    
    pthread_rwlock_unlock(&g_config_lock);
    
    LOG_AUDIT("Configuration loaded/reloaded successfully.");
}

void config_init(void) {
    config_reload();
}

void config_get_odbc_conn_str(char* out, size_t max_len) {
    pthread_rwlock_rdlock(&g_config_lock);
    snprintf(out, max_len, "%s", g_odbc_conn_str);
    pthread_rwlock_unlock(&g_config_lock);
}

void config_get_api_url(char* out, size_t max_len) {
    pthread_rwlock_rdlock(&g_config_lock);
    snprintf(out, max_len, "%s", g_api_url);
    pthread_rwlock_unlock(&g_config_lock);
}

void config_get_api_user(char* out, size_t max_len) {
    pthread_rwlock_rdlock(&g_config_lock);
    snprintf(out, max_len, "%s", g_api_user);
    pthread_rwlock_unlock(&g_config_lock);
}

void config_get_api_pass(char* out, size_t max_len) {
    pthread_rwlock_rdlock(&g_config_lock);
    snprintf(out, max_len, "%s", g_api_pass);
    pthread_rwlock_unlock(&g_config_lock);
}

bool config_get_access_log(void) {
    pthread_rwlock_rdlock(&g_config_lock);
    bool val = g_access_log;
    pthread_rwlock_unlock(&g_config_lock);
    return val;
}

size_t config_get_num_threads(void) {
    pthread_rwlock_rdlock(&g_config_lock);
    size_t val = g_num_threads;
    pthread_rwlock_unlock(&g_config_lock);
    return val;
}

size_t config_get_max_queue_size(void) {
    pthread_rwlock_rdlock(&g_config_lock);
    size_t val = g_max_queue_size;
    pthread_rwlock_unlock(&g_config_lock);
    return val;
}
