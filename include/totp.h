#pragma once

#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>
#include <stdbool.h>

/** \brief Generates an SVG QR code for TOTP authenticator registration. */
void totp_generate_svg(const char* user, int* out_status, struct evbuffer* out_buf);

/** \brief Validates a TOTP code for a given user. */
bool is_valid_totp(const char* username, const char* totp_code);
