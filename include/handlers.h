#pragma once
#include <json-c/json.h>
#include <event2/http.h>
#include "validation.h"

/**
 * \file handlers.h
 * \brief HTTP request handlers for the libevent web server.
 */

/** \brief Validation schema context for /customer route. */
extern const ValidationContext CustomerContext;

/** \brief Validation schema context for /sales route. */
extern const ValidationContext SalesContext;

/** \brief Handles /ping requests for liveness checks. */
void ping_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /version requests. */
void version_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /sysinfo requests. */
void sysinfo_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /rsysinfo requests. */
void rsysinfo_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handler to test UUIDv4 generation */
void uuid_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /login requests. */
void login_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);
/** \brief Validation schema context for /login route. */
extern const ValidationContext LoginContext;

/** \brief Handles /customer requests (external REST integration). */
void rcustomer_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /customer_get requests (database integration). */
void customer_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /sales requests. */
void sales_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /shippers requests. */
void shippers_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /products requests. */
void products_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /metrics requests. Returns plain text Prometheus format instead of JSON. */
void metrics_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /getqr requests for TOTP registration. */
void getqr_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);

/** \brief Handles /verifytotp requests for TOTP validation. */
void verifytotp_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);
/** \brief Validation schema context for /verifytotp route. */
extern const ValidationContext VerifyTotpContext;

/** \brief Handles /employee requests. */
void employee_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);
/** \brief Validation schema context for /employee route. */
extern const ValidationContext EmployeeContext;

/** \brief Handles /prodget requests. */
void prodget_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);
/** \brief Validation schema context for /prodget route. */
extern const ValidationContext ProdgetContext;

/** \brief Handles /customers requests. */
void customers_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);
/** \brief Validation schema context for /customers route. */
extern const ValidationContext CustomersContext;

/** \brief Handles /sales requests. */
void sales_pgsql_handler(struct json_object* body, void* arg, int* out_status, struct evbuffer* out_buf);