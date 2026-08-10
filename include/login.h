#pragma once
#include <json-c/json.h>

/**
 * \file login.h
 * \brief Login service module
 */

/**
 * \brief Authenticates a user against the remote login provider
 * \return A newly allocated json_object, or nullptr on failure.
 * \note Caller must assign the return value to a variable marked with [[gnu::cleanup(cleanup_json_object)]] to prevent memory leaks.
 */
struct json_object* login_service_authenticate(const char* username, const char* password, long* out_http_code);
