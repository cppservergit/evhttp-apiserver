#pragma once
#include <json-c/json.h>
#include <event2/http.h>
#include "validation.h"

/**
 * \file handlers.h
 * \brief HTTP request handlers for the libevent web server.
 */

extern const ValidationContext CustomerContext;
extern const ValidationContext SalesContext;

/** \brief Handles /ping requests for liveness checks. */
struct json_object* ping_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /version requests. */
struct json_object* version_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /sysinfo requests. */
struct json_object* sysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /rsysinfo requests. */
struct json_object* rsysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handler to test UUIDv4 generation */
struct json_object* uuid_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /login requests. */
struct json_object* login_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);
extern const ValidationContext LoginContext;

/** \brief Extracts the client IP from the request, favoring X-Forwarded-For if present. */
const char* extract_client_ip(struct evhttp_request* req);

/** \brief Retrieves the internal JWT-authenticated username, if present. */
const char* get_user(struct evhttp_request* req);

/** \brief Retrieves the internal JWT-authenticated session ID, if present. */
const char* get_session_id(struct evhttp_request* req);

/** \brief Handles /customer requests (external REST integration). */
struct json_object* customer_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /customer_get requests (database integration). */
struct json_object* customer_get_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /sales requests. */
struct json_object* sales_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /shippers requests. */
struct json_object* shippers_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /products requests. */
struct json_object* products_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \brief Handles /metrics requests. Returns plain text Prometheus format instead of JSON. */
struct evbuffer* metrics_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);
