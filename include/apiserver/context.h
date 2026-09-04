#pragma once

/**
 * \file context.h
 * \brief Thread-local request context and metadata manager.
 *
 * Provides thread-local storage accessors to propagate request-scoped metadata
 * (authenticated user, session ID, client IP address, request URI, and response
 * Content-Type override) across worker threads during HTTP request processing.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Populates thread-local request context with metadata.
 * \param user Authenticated username string, or nullptr to leave empty.
 * \param session Unique session/trace ID string, or nullptr to leave empty.
 * \param client_ip Client IP address string, or nullptr to leave empty.
 * \param uri Request URI path and query string, or nullptr to leave empty.
 */
void context_set(const char* user, const char* session, const char* client_ip, const char* uri);

/**
 * \brief Resets and clears all thread-local request context fields.
 */
void context_clear(void);

/**
 * \brief Retrieves the authenticated username for the current request.
 * \return Pointer to the null-terminated username string, or nullptr if not set.
 */
const char* context_get_user(void);

/**
 * \brief Retrieves the session or trace identifier for the current request.
 * \return Pointer to the null-terminated session ID string, or nullptr if not set.
 */
const char* context_get_session_id(void);

/**
 * \brief Retrieves the client IP address for the current request.
 * \return Pointer to the null-terminated client IP string, or nullptr if not set.
 */
const char* context_get_client_ip(void);

/**
 * \brief Sets a custom response Content-Type header in the thread-local context.
 * \param ctype MIME type string (e.g. "application/json", "text/plain"), or nullptr to clear.
 */
void context_set_content_type(const char* ctype);

/**
 * \brief Retrieves the response Content-Type override for the current request.
 * \return Pointer to the custom MIME type string, or nullptr if not set.
 */
const char* context_get_content_type(void);

#ifdef __cplusplus
}
#endif
