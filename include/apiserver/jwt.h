#pragma once
#include <time.h>

/**
 * \file jwt.h
 * \brief JSON Web Token (JWT) decoding utilities.
 */

/** 
 * \brief Decodes the base64 payload from a JWT token into a provided buffer.
 * \param jwt The raw JWT string.
 * \param out The buffer to write the decoded JSON string to.
 * \param out_maxlen The maximum length of the output buffer.
 * \return True on success, False on failure.
 */
#include <stdbool.h>
bool jwt_decode_payload(const char* jwt, char* out, size_t out_maxlen);

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
 * \param out_jwt The pre-allocated output buffer where the JWT will be written.
 * \param out_jwt_size The maximum size of the output buffer.
 * \return True on success, False on failure.
 */
bool jwt_create(const char* username, const char* session_id, const char* secret_hex, long timeout_seconds, char* out_jwt, size_t out_jwt_size);

/** \brief Return code for successful JWT verification. */
constexpr int JWT_OK = 0;
/** \brief Returned when the JWT is valid but has expired. */
constexpr int JWT_ERR_EXPIRED = 1;
/** \brief Returned when the JWT signature is invalid or malformed. */
constexpr int JWT_ERR_INVALID = 2;

/**
 * \brief Verifies a JWT token signature and expiration, and extracts claims.
 * \return JWT_OK (0) if valid, JWT_ERR_EXPIRED if expired, JWT_ERR_INVALID if missing/bad signature.
 */
int jwt_verify(const char* token, const char* secret_hex, char* out_username, size_t out_uname_size, char* out_session_id, size_t out_sess_size);
