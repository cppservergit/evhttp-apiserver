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
static char g_login_provider[MAX_CONFIG_STR] = {0};
static char g_login_uri[MAX_CONFIG_STR] = {0};
static char g_jwt_secret[MAX_CONFIG_STR] = {0};
static char g_remote_api_key[MAX_CONFIG_STR] = {0};
static char g_telemetry_api_key[MAX_CONFIG_STR] = {0};
static long g_jwt_timeout_seconds = 3600;
static char g_trust_proxy_ip[MAX_CONFIG_STR] = {0};
static bool g_access_log = true;
static size_t g_num_threads = 0;
static size_t g_max_queue_size = 10000; // Default backpressure limit
static size_t g_fast_pool_percentage = 25; // Default fast pool allocation

static void trim_newline(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len-1] == '\n' || str[len-1] == '\r')) {
        str[len-1] = '\0';
        len--;
    }
}

static bool load_env_file(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) return false;

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
    return true;
}

static bool locate_and_load_env(void) {
    char exe_path[PATH_MAX] = {0};
    char env_path[PATH_MAX] = {0};
    if (readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) != -1) {
        char* dir = dirname(exe_path);
        snprintf(env_path, sizeof(env_path), "%s/apiserver.env", dir);
        if (load_env_file(env_path)) return true;
    }
    return load_env_file("apiserver.env");
}

static bool parse_boolean_env(const char* env_val, bool default_val) {
    if (!env_val) return default_val;
    if (strcasecmp(env_val, "false") == 0 || strcasecmp(env_val, "0") == 0 || 
        strcasecmp(env_val, "off") == 0 || strcasecmp(env_val, "no") == 0) {
        return false;
    }
    return true;
}

static void apply_config_updates(size_t num_threads, size_t max_queue, size_t fast_pool, bool access_log) {
    pthread_rwlock_wrlock(&g_config_lock);
    
    if (getenv("ODBC_CONN_STR")) snprintf(g_odbc_conn_str, sizeof(g_odbc_conn_str), "%s", getenv("ODBC_CONN_STR"));
    if (getenv("API_URL")) snprintf(g_api_url, sizeof(g_api_url), "%s", getenv("API_URL"));
    if (getenv("API_USER")) snprintf(g_api_user, sizeof(g_api_user), "%s", getenv("API_USER"));
    if (getenv("API_PASS")) snprintf(g_api_pass, sizeof(g_api_pass), "%s", getenv("API_PASS"));
    
    if (getenv("LOGIN_PROVIDER")) snprintf(g_login_provider, sizeof(g_login_provider), "%s", getenv("LOGIN_PROVIDER"));
    if (getenv("LOGIN_URI")) snprintf(g_login_uri, sizeof(g_login_uri), "%s", getenv("LOGIN_URI"));
    if (getenv("JWT_SECRET")) snprintf(g_jwt_secret, sizeof(g_jwt_secret), "%s", getenv("JWT_SECRET"));
    if (getenv("REMOTE_API_KEY")) snprintf(g_remote_api_key, sizeof(g_remote_api_key), "%s", getenv("REMOTE_API_KEY"));
    else g_remote_api_key[0] = '\0';
    
    if (getenv("TELEMETRY_API_KEY")) snprintf(g_telemetry_api_key, sizeof(g_telemetry_api_key), "%s", getenv("TELEMETRY_API_KEY"));
    else g_telemetry_api_key[0] = '\0';
    
    if (getenv("TRUST_PROXY_IP")) snprintf(g_trust_proxy_ip, sizeof(g_trust_proxy_ip), "%s", getenv("TRUST_PROXY_IP"));
    else g_trust_proxy_ip[0] = '\0';
    
    if (getenv("JWT_TIMEOUT_SECONDS")) {
        g_jwt_timeout_seconds = strtol(getenv("JWT_TIMEOUT_SECONDS"), NULL, 10);
    }

    
    static bool is_first_load = true;
    if (!is_first_load && g_num_threads != num_threads && (g_num_threads != 0 || num_threads != 0)) {
        LOG_WARN("NUM_THREADS changed from %zu to %zu. Requires full restart.", g_num_threads, num_threads);
    }

    g_access_log = access_log;
    g_num_threads = num_threads;
    g_max_queue_size = max_queue;
    g_fast_pool_percentage = fast_pool;
    
    bool was_first = is_first_load;
    is_first_load = false;
    pthread_rwlock_unlock(&g_config_lock);
    
    if (was_first) LOG_INFO("Configuration loaded successfully on startup.");
    else LOG_AUDIT("Configuration hot-reloaded successfully.");
}

void config_reload(void) {
    locate_and_load_env();

    const char* env_odbc = getenv("ODBC_CONN_STR");
    const char* env_url = getenv("API_URL");
    const char* env_user = getenv("API_USER");
    const char* env_pass = getenv("API_PASS");

    if (!env_odbc || !env_url || !env_user || !env_pass) {
        if (!env_odbc) LOG_FATAL("Missing required config: ODBC_CONN_STR");
        if (!env_url) LOG_FATAL("Missing required config: API_URL");
        if (!env_user) LOG_FATAL("Missing required config: API_USER");
        if (!env_pass) LOG_FATAL("Missing required config: API_PASS");
        return;
    }
    
    size_t num_threads = getenv("NUM_THREADS") ? strtoul(getenv("NUM_THREADS"), nullptr, 10) : 0;
    size_t max_queue = getenv("MAX_QUEUE_SIZE") ? strtoul(getenv("MAX_QUEUE_SIZE"), nullptr, 10) : 10000;
    size_t fast_pool = getenv("FAST_POOL_PERCENTAGE") ? strtoul(getenv("FAST_POOL_PERCENTAGE"), nullptr, 10) : 25;
    if (fast_pool > 100) fast_pool = 100;
    
    bool access_log = parse_boolean_env(getenv("ACCESS_LOG"), true);
    
    apply_config_updates(num_threads, max_queue, fast_pool, access_log);
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

void config_get_login_provider(char* out, size_t max_len) {
    pthread_rwlock_rdlock(&g_config_lock);
    snprintf(out, max_len, "%s", g_login_provider);
    pthread_rwlock_unlock(&g_config_lock);
}

void config_get_login_uri(char* out, size_t max_len) {
    pthread_rwlock_rdlock(&g_config_lock);
    snprintf(out, max_len, "%s", g_login_uri);
    pthread_rwlock_unlock(&g_config_lock);
}


void config_get_jwt_secret(char* out, size_t max_len) {
    if (!out || max_len == 0) return;
    pthread_rwlock_rdlock(&g_config_lock);
    strncpy(out, g_jwt_secret, max_len - 1);
    out[max_len - 1] = '\0';
    pthread_rwlock_unlock(&g_config_lock);
}

void config_get_remote_api_key(char* out, size_t max_len) {
    if (!out || max_len == 0) return;
    pthread_rwlock_rdlock(&g_config_lock);
    strncpy(out, g_remote_api_key, max_len - 1);
    out[max_len - 1] = '\0';
    pthread_rwlock_unlock(&g_config_lock);
}

void config_get_telemetry_api_key(char* out, size_t max_len) {
    if (!out || max_len == 0) return;
    pthread_rwlock_rdlock(&g_config_lock);
    strncpy(out, g_telemetry_api_key, max_len - 1);
    out[max_len - 1] = '\0';
    pthread_rwlock_unlock(&g_config_lock);
}

void config_get_trust_proxy_ip(char* buf, size_t max_len) {
    pthread_rwlock_rdlock(&g_config_lock);
    strncpy(buf, g_trust_proxy_ip, max_len - 1);
    buf[max_len - 1] = '\0';
    pthread_rwlock_unlock(&g_config_lock);
}

long config_get_jwt_timeout_seconds(void) {
    pthread_rwlock_rdlock(&g_config_lock);
    long res = g_jwt_timeout_seconds;
    pthread_rwlock_unlock(&g_config_lock);
    return res;
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

size_t config_get_fast_pool_percentage(void) {
    pthread_rwlock_rdlock(&g_config_lock);
    size_t val = g_fast_pool_percentage;
    pthread_rwlock_unlock(&g_config_lock);
    return val;
}
