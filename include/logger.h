#pragma once

#include <stdio.h>
#include <stdarg.h>

/**
 * \file logger.h
 * \brief Thread-safe JSON structured logging framework.
 */

/** \brief Available severity levels for structured logging. */
typedef enum {
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_AUDIT,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
    LOG_LEVEL_DEBUG
} LogLevel;

#define LOG_INFO(...)  logger_log(LOG_LEVEL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  logger_log(LOG_LEVEL_WARN,  __VA_ARGS__)
#define LOG_AUDIT(...) logger_log(LOG_LEVEL_AUDIT, __VA_ARGS__)
#define LOG_ERROR(...) logger_log(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_FATAL(...) logger_log(LOG_LEVEL_FATAL, __VA_ARGS__)
#define LOG_DEBUG(...) logger_log(LOG_LEVEL_DEBUG, __VA_ARGS__)

/**
 * \brief Writes a structured JSON log entry to stderr.
 * \param level The severity level.
 * \param format printf-style format string.
 */
__attribute__((format(printf, 2, 3)))
void logger_log(LogLevel level, const char* format, ...);

/**
 * \brief Sets the thread-local request ID for distributed tracing (MDC).
 * \param req_id Pointer to the request ID string (must remain valid while set, e.g. from libevent headers).
 */
void logger_set_request_id(const char* req_id);

/**
 * \brief Clears the thread-local request ID.
 */
void logger_clear_request_id(void);

/**
 * \brief Initializes the asynchronous logger background thread.
 */
void logger_init(void);

/**
 * \brief Flushes remaining logs and shuts down the logger thread.
 */
void logger_shutdown(void);
