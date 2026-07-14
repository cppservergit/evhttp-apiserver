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
