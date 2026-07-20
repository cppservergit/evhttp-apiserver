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
void ping_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /version requests. */
void version_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /sysinfo requests. */
void sysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /rsysinfo requests. */
void rsysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handler to test UUIDv4 generation */
void uuid_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /login requests. */
void login_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);
extern const ValidationContext LoginContext;

/** \brief Extracts the client IP from the request, favoring X-Forwarded-For if present. */
const char* extract_client_ip(struct evhttp_request* req);

/** \brief Retrieves the internal JWT-authenticated username, if present. */
const char* get_user(struct evhttp_request* req);

/** \brief Retrieves the internal JWT-authenticated session ID, if present. */
const char* get_session_id(struct evhttp_request* req);

/** \brief Sets the authenticated identity for the current thread context. */
void handlers_set_identity(const char* user, const char* session);

/** \brief Clears the authenticated identity from the current thread context. */
void handlers_clear_identity(void);

/** \brief Checks if the request comes from a trusted proxy and optionally returns the peer IP. */
bool is_trusted_proxy(struct evhttp_request* req, const char** out_peer_ip);

/** \brief Handles /customer requests (external REST integration). */
void customer_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /customer_get requests (database integration). */
void customer_get_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /sales requests. */
void sales_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /shippers requests. */
void shippers_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /products requests. */
void products_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /metrics requests. Returns plain text Prometheus format instead of JSON. */
void metrics_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);

/** \brief Handles /getqr requests for TOTP registration. */
void getqr_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);
