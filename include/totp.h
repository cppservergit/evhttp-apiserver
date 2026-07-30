#pragma once

/**
 * \file totp.h
 * \brief TOTP generation and validation framework.
 */

#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>
#include <stdbool.h>

/** \brief Generates an SVG QR code for TOTP authenticator registration. */
void totp_generate_svg(const char* user, int* out_status, struct evbuffer* out_buf);

/** \brief Validates a TOTP code for a given user. */
bool is_valid_totp(const char* username, const char* totp_code);

/** 
 * \brief Generates a cryptographically secure random base32 secret (20 bytes of entropy). 
 * \param out_secret Pointer to the buffer where the null-terminated base32 string will be written.
 * \param out_maxlen The maximum length of the output buffer (must be exactly 33 for a 20-byte secret).
 * \return true on success, false if the buffer is too small or improperly sized.
 */
bool totp_generate_base32_secret(char* out_secret, size_t out_maxlen);
