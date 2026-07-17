#pragma once

#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>

/** \brief Generates an SVG QR code for TOTP authenticator registration. */
struct evbuffer* totp_generate_svg(const char* user, int* out_status, const char** out_status_txt);
