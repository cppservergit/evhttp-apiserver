#pragma once
#include <stddef.h>
#include <stdbool.h>

/**
 * \file config.h
 * \brief Application configuration and hot-reloading subsystem.
 */

/** \brief Maximum length for a configuration string value. */
constexpr int MAX_CONFIG_STR = 1024;

/** \brief Initializes the configuration subsystem. */
void config_init(void);

/** \brief Reloads configuration from the environment/file dynamically (e.g., on SIGHUP). */
void config_reload(void);

/** \brief Checks if access logging is enabled. \return true if enabled, false otherwise. */
bool config_get_access_log(void);

/** \brief Retrieves the configured number of worker threads, or 0 if not set. */
size_t config_get_num_threads(void);

/** \brief Retrieves the configured server port. */
int config_get_server_port(void);

/** \brief Retrieves the maximum allowed size of the background task queue. */
size_t config_get_max_queue_size(void);

/** \brief Retrieves the maximum allowed payload size for HTTP requests/responses. */
size_t config_get_max_payload_size(void);

/** \brief Retrieves the configured percentage of threads to assign to the fast pool (0-100). */
size_t config_get_fast_pool_percentage(void);

/** 
 * \brief Safely retrieves the ODBC connection string.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
const char* config_get_odbc_conn_str(int db_id);

/** 
 * \brief Safely retrieves the remote API URL.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
const char* config_get_api_url(void);

/** 
 * \brief Safely retrieves the remote API username.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
const char* config_get_api_user(void);

/** 
 * \brief Safely retrieves the remote API password.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
const char* config_get_api_pass(void);

/** \brief Retrieves the remote login provider URL. */
const char* config_get_login_provider(void);

/** \brief Retrieves the remote login URI. */
const char* config_get_login_uri(void);

/** \brief Retrieves the JWT secret key. */
const char* config_get_jwt_secret(void);

/** \brief Retrieves the remote API key. */
const char* config_get_remote_api_key(void);

/** \brief Retrieves the telemetry API key. */
const char* config_get_telemetry_api_key(void);

/** \brief Retrieves the JWT expiration timeout in seconds. */
long config_get_jwt_timeout_seconds(void);

/** \brief Retrieves the trusted proxy IP for X-Forwarded-For processing. */
const char* config_get_trust_proxy_ip(void);

/** \brief Checks if a CORS Origin is allowed by configuration. */
bool config_is_origin_allowed(const char* origin);

/** \brief Retrieves the uploads directory. */
const char* config_get_uploads_dir(void);

