#include <apiserver/config.h>
#include <apiserver/logger.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <unistd.h>
#include <libgen.h>
#include <linux/limits.h>
#include <stdatomic.h>
#include <ctype.h>
#include <errno.h>

static unsigned long safe_strtoul_env(const char* env_val, unsigned long default_val, unsigned long max_val) {
    if (!env_val || !*env_val) return default_val;
    char* endptr = nullptr;
    errno = 0;
    unsigned long val = strtoul(env_val, &endptr, 10);
    if (errno != 0 || *endptr != '\0') {
        LOG_WARN("Malformed numeric configuration '%s'. Using default %lu.", env_val, default_val);
        return default_val;
    }
    if (val > max_val) {
        LOG_WARN("Configuration value %lu exceeds maximum %lu. Clamping.", val, max_val);
        return max_val;
    }
    return val;
}

static long safe_strtol_env(const char* env_val, long default_val, long min_val, long max_val) {
    if (!env_val || !*env_val) return default_val;
    char* endptr = nullptr;
    errno = 0;
    long val = strtol(env_val, &endptr, 10);
    if (errno != 0 || *endptr != '\0') {
        LOG_WARN("Malformed numeric configuration '%s'. Using default %ld.", env_val, default_val);
        return default_val;
    }
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static char g_odbc_conn_strs[4][MAX_CONFIG_STR] = {0};
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
static char g_allowed_origin[MAX_CONFIG_STR] = {0};
static char g_allowed_origin_copy[MAX_CONFIG_STR] = {0};
static char* g_allowed_origin_ptrs[64] = {0};
static size_t g_allowed_origin_count = 0;
static _Atomic bool g_access_log = true;
static size_t g_num_threads = 0;
static size_t g_max_queue_size = 10000; // Default backpressure limit
static size_t g_fast_pool_percentage = 25; // Default fast pool allocation
static size_t g_max_payload_size = 5242880; // Default 5MB payload max
static char g_uploads_dir[MAX_CONFIG_STR] = {0};
static int g_server_port = 8080;

static char* trim_whitespace(char* str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

static bool parse_boolean_env(const char* env_val, bool default_val);
static bool is_first_load = true;

static bool load_env_file(const char* filepath, bool hot_reload) {
    FILE* f = fopen(filepath, "r");
    if (!f) return false;

    char line[MAX_CONFIG_STR];
    while (fgets(line, sizeof(line), f)) {
        char* trimmed_line = trim_whitespace(line);
        if (trimmed_line[0] == '#' || trimmed_line[0] == '\0') continue;
        
        char* eq = strchr(trimmed_line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char* key = trim_whitespace(trimmed_line);
        char* val = trim_whitespace(eq + 1);
        if (val[0] == '"' || val[0] == '\'') {
            char quote = val[0];
            val++;
            size_t len = strlen(val);
            if (len > 0 && val[len - 1] == quote) {
                val[len - 1] = '\0';
            }
        }
        
        if (hot_reload) {
            if (strcmp(key, "ACCESS_LOG") == 0) {
                bool access_log = parse_boolean_env(val, true);
                atomic_store_explicit(&g_access_log, access_log, memory_order_relaxed);
            }
        } else if (key[0] != '\0') {
            setenv(key, val, 1);
        }
    }
    (void)fclose(f);
    return true;
}

static bool locate_and_load_env(bool hot_reload) {
    char exe_path[PATH_MAX] = {0};
    if (readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) != -1) {
        char env_path[PATH_MAX] = {0};
        const char* dir = dirname(exe_path);
        (void)snprintf(env_path, sizeof(env_path), "%s/apiserver.env", dir);
        if (load_env_file(env_path, hot_reload)) return true;
    }
    return load_env_file("apiserver.env", hot_reload);
}

static bool parse_boolean_env(const char* env_val, bool default_val) {
    if (!env_val) return default_val;
    if (strcasecmp(env_val, "false") == 0 || strcasecmp(env_val, "0") == 0 || 
        strcasecmp(env_val, "off") == 0 || strcasecmp(env_val, "no") == 0) {
        return false;
    }
    return true;
}



// WARNING: This function updates multiple static string buffers non-atomically.
// It is currently thread-safe only because it is called during startup
// (or hot reload of ACCESS_LOG which bypasses this). If config reloading
// is expanded to these variables in the future, explicit synchronization
// (e.g. read-write locks or RCU) MUST be implemented to prevent race conditions.
static void apply_initial_config_vars(void) {
    for (int i = 0; i < 4; i++) {
        char env_key[8];
        (void)snprintf(env_key, sizeof(env_key), "DB_%d", i);
        const char* val = getenv(env_key);
        if (val) {
            (void)snprintf(g_odbc_conn_strs[i], sizeof(g_odbc_conn_strs[i]), "%s", val);
        }
    }
    if (getenv("API_URL")) (void)snprintf(g_api_url, sizeof(g_api_url), "%s", getenv("API_URL"));
    if (getenv("API_USER")) (void)snprintf(g_api_user, sizeof(g_api_user), "%s", getenv("API_USER"));
    if (getenv("API_PASS")) (void)snprintf(g_api_pass, sizeof(g_api_pass), "%s", getenv("API_PASS"));
    
    if (getenv("LOGIN_PROVIDER")) (void)snprintf(g_login_provider, sizeof(g_login_provider), "%s", getenv("LOGIN_PROVIDER"));
    if (getenv("LOGIN_URI")) (void)snprintf(g_login_uri, sizeof(g_login_uri), "%s", getenv("LOGIN_URI"));
    if (getenv("JWT_SECRET")) (void)snprintf(g_jwt_secret, sizeof(g_jwt_secret), "%s", getenv("JWT_SECRET"));
    
    if (getenv("REMOTE_API_KEY")) (void)snprintf(g_remote_api_key, sizeof(g_remote_api_key), "%s", getenv("REMOTE_API_KEY"));
    else g_remote_api_key[0] = '\0';
    
    if (getenv("TELEMETRY_API_KEY")) (void)snprintf(g_telemetry_api_key, sizeof(g_telemetry_api_key), "%s", getenv("TELEMETRY_API_KEY"));
    else g_telemetry_api_key[0] = '\0';
    
    if (getenv("TRUST_PROXY_IP")) (void)snprintf(g_trust_proxy_ip, sizeof(g_trust_proxy_ip), "%s", getenv("TRUST_PROXY_IP"));
    else g_trust_proxy_ip[0] = '\0';
    
    if (getenv("CORS_ALLOWED_ORIGINS")) (void)snprintf(g_allowed_origin, sizeof(g_allowed_origin), "%s", getenv("CORS_ALLOWED_ORIGINS"));
    else g_allowed_origin[0] = '\0';
    
    if (getenv("JWT_TIMEOUT_SECONDS")) {
        g_jwt_timeout_seconds = safe_strtol_env(getenv("JWT_TIMEOUT_SECONDS"), 3600, 1, 86400 * 30);
    }
    
    if (getenv("UPLOADS_DIR")) (void)snprintf(g_uploads_dir, sizeof(g_uploads_dir), "%s", getenv("UPLOADS_DIR"));
    else g_uploads_dir[0] = '\0';
    
    if (g_allowed_origin[0] != '\0') {
        (void)snprintf(g_allowed_origin_copy, sizeof(g_allowed_origin_copy), "%s", g_allowed_origin);
        char* saveptr = nullptr;
        char* token = strtok_r(g_allowed_origin_copy, ",", &saveptr);
        while (token != nullptr && g_allowed_origin_count < 64) {
            token = trim_whitespace(token);
            if (token[0] != '\0') {
                g_allowed_origin_ptrs[g_allowed_origin_count++] = token;
            }
            token = strtok_r(nullptr, ",", &saveptr);
        }
    }
}

void config_reload(void) {
    if (!is_first_load) {
        locate_and_load_env(true);
        LOG_AUDIT("Configuration hot-reloaded successfully. Only ACCESS_LOG was updated.");
        return;
    }

    locate_and_load_env(false);

    const char* env_odbc = getenv("DB_0");
    const char* env_url = getenv("API_URL");
    const char* env_user = getenv("API_USER");
    const char* env_pass = getenv("API_PASS");
    const char* env_jwt_secret = getenv("JWT_SECRET");
    const char* env_jwt_timeout = getenv("JWT_TIMEOUT_SECONDS");
    const char* env_telemetry = getenv("TELEMETRY_API_KEY");

    if (!env_odbc || !env_url || !env_user || !env_pass || !env_jwt_secret || !env_jwt_timeout || !env_telemetry) {
        if (!env_odbc) LOG_ERROR("Missing required config: DB_0");
        if (!env_url)  LOG_ERROR("Missing required config: API_URL");
        if (!env_user) LOG_ERROR("Missing required config: API_USER");
        if (!env_pass) LOG_ERROR("Missing required config: API_PASS");
        if (!env_jwt_secret) LOG_ERROR("Missing required config: JWT_SECRET");
        if (!env_jwt_timeout) LOG_ERROR("Missing required config: JWT_TIMEOUT_SECONDS");
        if (!env_telemetry) LOG_ERROR("Missing required config: TELEMETRY_API_KEY");
        LOG_FATAL("One or more required configuration variables are missing. Aborting startup.");
    }
    
    g_num_threads = (size_t)safe_strtoul_env(getenv("NUM_THREADS"), 0, 1024);
    g_max_queue_size = (size_t)safe_strtoul_env(getenv("MAX_QUEUE_SIZE"), 10000, 1000000);
    g_fast_pool_percentage = (size_t)safe_strtoul_env(getenv("FAST_POOL_PERCENTAGE"), 25, 100);
    g_max_payload_size = (size_t)safe_strtoul_env(getenv("MAX_PAYLOAD_SIZE"), 5242880, 104857600);
    g_server_port = (int)safe_strtol_env(getenv("SERVER_PORT"), 8080, 1, 65535);
    
    bool access_log = parse_boolean_env(getenv("ACCESS_LOG"), true);
    atomic_store_explicit(&g_access_log, access_log, memory_order_relaxed);

    apply_initial_config_vars();
    
    unsetenv("JWT_SECRET");
    unsetenv("API_PASS");
    unsetenv("REMOTE_API_KEY");
    unsetenv("TELEMETRY_API_KEY");
    for (int i = 0; i < 4; i++) {
        char env_key[8];
        (void)snprintf(env_key, sizeof(env_key), "DB_%d", i);
        unsetenv(env_key);
    }
    
    is_first_load = false;
    LOG_INFO("Configuration loaded successfully on startup.");
}

void config_init(void) {
    config_reload();
}

const char* config_get_odbc_conn_str(int db_id) {
    if (db_id >= 0 && db_id < 4) return g_odbc_conn_strs[db_id];
    return "";
}

const char* config_get_api_url(void) {
    return g_api_url;
}

const char* config_get_api_user(void) {
    return g_api_user;
}

const char* config_get_api_pass(void) {
    return g_api_pass;
}

const char* config_get_login_provider(void) {
    return g_login_provider;
}

const char* config_get_login_uri(void) {
    return g_login_uri;
}


const char* config_get_jwt_secret(void) {
    return g_jwt_secret;
}

const char* config_get_remote_api_key(void) {
    return g_remote_api_key;
}

const char* config_get_telemetry_api_key(void) {
    return g_telemetry_api_key;
}

const char* config_get_trust_proxy_ip(void) {
    return g_trust_proxy_ip;
}

const char* config_get_uploads_dir(void) {
    return g_uploads_dir;
}

long config_get_jwt_timeout_seconds(void) {
    return g_jwt_timeout_seconds;
}

bool config_get_access_log(void) {
    return atomic_load_explicit(&g_access_log, memory_order_relaxed);
}

int config_get_server_port(void) {
    return g_server_port;
}

size_t config_get_num_threads(void) {
    return g_num_threads;
}

size_t config_get_max_queue_size(void) {
    return g_max_queue_size;
}

size_t config_get_fast_pool_percentage(void) {
    return g_fast_pool_percentage;
}

size_t config_get_max_payload_size(void) {
    return g_max_payload_size;
}

bool config_is_origin_allowed(const char* origin) {
    if (!origin || g_allowed_origin_count == 0) return false;
    
    for (size_t i = 0; i < g_allowed_origin_count; i++) {
        if (strcmp(g_allowed_origin_ptrs[i], origin) == 0) {
            return true;
        }
    }
    return false;
}
