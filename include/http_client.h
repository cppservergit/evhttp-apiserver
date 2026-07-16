#pragma once
#include <json-c/json.h>

/**
 * \file http_client.h
 * \brief Libcurl wrapper for HTTP client requests.
 */

/** \brief Initializes the thread-local libcurl handle (must be called once per thread). */
void http_client_init_thread(void);

/** \brief Cleans up the thread-local libcurl handle. */
void http_client_cleanup_thread(void);

/** 
 * \brief Performs an HTTP GET request and parses the response as JSON.
 * \param base_url The base URL (e.g. from config).
 * \param uri The URI to append to the base URL.
 * \param headers Array of HTTP headers, or NULL.
 * \param num_headers Number of headers in the array.
 * \param out_http_code Pointer to store the HTTP status code.
 * \return A newly allocated json_object, or NULL on failure.
 */
struct json_object* http_client_get_json(const char* base_url, const char* uri, const char** headers, int num_headers, long* out_http_code);

/** 
 * \brief Performs an HTTP POST request and parses the response as JSON.
 * \param base_url The base URL (e.g. from config).
 * \param uri The URI to append to the base URL.
 * \param body The POST payload body.
 * \param headers Array of HTTP headers, or NULL.
 * \param num_headers Number of headers in the array.
 * \param out_http_code Pointer to store the HTTP status code.
 * \return A newly allocated json_object, or NULL on failure.
 */
struct json_object* http_client_post_json(const char* base_url, const char* uri, const char* body, const char** headers, int num_headers, long* out_http_code);
