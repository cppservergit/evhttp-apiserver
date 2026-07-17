#pragma once
#include <json-c/json.h>

/**
 * \file login.h
 * \brief Login service module
 */

/**
 * \brief Authenticates a user against the remote login provider
 */
struct json_object* login_service_authenticate(const char* username, const char* password, long* out_http_code);
