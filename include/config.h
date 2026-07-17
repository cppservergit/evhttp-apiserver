#pragma once
#include <stddef.h>
#include <stdbool.h>

/**
 * \file config.h
 * \brief Application configuration and hot-reloading subsystem.
 */

/** \brief Maximum length for a configuration string value. */
#define MAX_CONFIG_STR 1024

/** \brief Initializes the configuration subsystem. */
void config_init(void);

/** \brief Reloads configuration from the environment/file dynamically (e.g., on SIGHUP). */
void config_reload(void);

/** \brief Checks if access logging is enabled. \return true if enabled, false otherwise. */
bool config_get_access_log(void);

/** \brief Retrieves the configured number of worker threads, or 0 if not set. */
size_t config_get_num_threads(void);

/** \brief Retrieves the maximum allowed size of the background task queue. */
size_t config_get_max_queue_size(void);

/** \brief Retrieves the configured percentage of threads to assign to the fast pool (0-100). */
size_t config_get_fast_pool_percentage(void);

/** 
 * \brief Safely retrieves the ODBC connection string.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
void config_get_odbc_conn_str(char* out, size_t max_len);

/** 
 * \brief Safely retrieves the remote API URL.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
void config_get_api_url(char* out, size_t max_len);

/** 
 * \brief Safely retrieves the remote API username.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
void config_get_api_user(char* out, size_t max_len);

/** 
 * \brief Safely retrieves the remote API password.
 * \param out Buffer to copy the string into.
 * \param max_len Size of the buffer.
 */
void config_get_api_pass(char* out, size_t max_len);

void config_get_login_provider(char* out, size_t max_len);
void config_get_login_uri(char* out, size_t max_len);
void config_get_login_payload(char* out, size_t max_len);
void config_get_jwt_secret(char* out, size_t max_len);
void config_get_remote_api_key(char* out, size_t max_len);
long config_get_jwt_timeout_seconds(void);

/** \brief Retrieves the trusted proxy IP for X-Forwarded-For processing. */
void config_get_trust_proxy_ip(char* buf, size_t max_len);
