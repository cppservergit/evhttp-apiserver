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

/** \brief Generates a cryptographically secure UUIDv4 string into out (which must be at least 37 bytes). */
void generate_uuidv4(char out[37]);

/** 
 * \brief Creates and signs a new JWT token using HMAC-SHA256.
 * \param username The username to include in the payload.
 * \param session_id The session ID to include in the payload.
 * \param secret_hex The 64-character hex-encoded secret key.
 * \param timeout_seconds The number of seconds until the token expires.
 * \return A newly allocated string containing the JWT, or NULL on failure. Caller must free.
 */
char* jwt_create(const char* username, const char* session_id, const char* secret_hex, long timeout_seconds);
