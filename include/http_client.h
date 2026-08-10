#pragma once
#include <json-c/json.h>

/**
 * \file http_client.h
 * \brief Libcurl wrapper for HTTP client requests.
 */

/** \brief Initializes the thread-local libcurl handle (must be called once per thread). */
void http_client_init_thread(void);

/** 
 * \brief Cleans up the thread-local libcurl handle.
 * \note This is currently an intentional no-op for API symmetry. 
 * Curl handles are automatically destroyed by the pthread_key destructor.
 */
void http_client_cleanup_thread(void);

/** 
 * \brief Performs an HTTP GET request and parses the response as JSON.
 * \param base_url The base URL (e.g. from config).
 * \param uri The URI to append to the base URL.
 * \param headers Array of HTTP headers, or nullptr.
 * \param num_headers Number of headers in the array.
 * \param out_http_code Pointer to store the HTTP status code.
 * \return A newly allocated json_object, or nullptr on failure.
 * \note Caller must assign the return value to a variable marked with [[gnu::cleanup(cleanup_json_object)]] to prevent memory leaks.
 */
struct json_object* http_client_get_json(const char* base_url, const char* uri, const char** headers, int num_headers, long* out_http_code);

/** 
 * \brief Performs an HTTP POST request and parses the response as JSON.
 * \param base_url The base URL (e.g. from config).
 * \param uri The URI to append to the base URL.
 * \param body The POST payload body.
 * \param headers Array of HTTP headers, or nullptr.
 * \param num_headers Number of headers in the array.
 * \param out_http_code Pointer to store the HTTP status code.
 * \return A newly allocated json_object, or nullptr on failure.
 * \note Caller must assign the return value to a variable marked with [[gnu::cleanup(cleanup_json_object)]] to prevent memory leaks.
 */
struct json_object* http_client_post_json(const char* base_url, const char* uri, const char* body, const char** headers, int num_headers, long* out_http_code);
