#pragma once

/**
 * \file thread_error.h
 * \brief Thread-local error state management for lock-free error handling.
 */

#define MAX_ERR_MSG_LEN 1024

/** \brief Error severity levels for thread-local error state. */
typedef enum {
    TL_ERR_NONE = 0,
    TL_ERR_WARN,
    TL_ERR_ERROR
} ThreadErrorLevel;

/** 
 * \brief Sets the thread-local error state with a formatted message.
 * \param level The severity of the error.
 * \param format The printf-style format string.
 */
void set_thread_error(ThreadErrorLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));

/** \brief Retrieves the current thread-local error message. \return The error string, or empty string if none. */
const char* get_thread_error_msg(void);

/** \brief Retrieves the current thread-local error level. \return The ThreadErrorLevel. */
ThreadErrorLevel get_thread_error_level(void);

/** \brief Clears the thread-local error state. */
void clear_thread_error(void);
