#pragma once
#include <time.h>

/**
 * \file jwt.h
 * \brief JSON Web Token (JWT) decoding utilities.
 */

/** 
 * \brief Decodes the base64 payload from a JWT token.
 * \param jwt The raw JWT string.
 * \return A malloc'd string containing the payload (must be freed), or NULL on failure.
 */
char* jwt_decode_payload(const char* jwt);

/** 
 * \brief Extracts the 'exp' (expiration) claim from a JWT token.
 * \param jwt The raw JWT string.
 * \return The expiration epoch time, or 0 if missing/invalid.
 */
time_t jwt_get_expiration(const char* jwt);
