#pragma once

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <json-c/json.h>
#include <stdlib.h>

/**
 * \file raii.h
 * \brief Resource Acquisition Is Initialization (RAII) memory cleanup macros using GCC's __attribute__((cleanup)).
 */

static inline void cleanup_event_config(struct event_config** cfg) { if (*cfg != nullptr) event_config_free(*cfg); }
static inline void cleanup_event_base(struct event_base** base) { if (*base != nullptr) event_base_free(*base); }
static inline void cleanup_evhttp(struct evhttp** http) { if (*http != nullptr) evhttp_free(*http); }
static inline void cleanup_json_tokener(struct json_tokener** tok) { if (*tok != nullptr) json_tokener_free(*tok); }
static inline void cleanup_json_object(struct json_object** obj) { if (*obj != nullptr) json_object_put(*obj); }

/** \brief Scoped auto-cleanup for event_config. */
#define raii_event_config [[gnu::cleanup(cleanup_event_config)]] struct event_config*
/** \brief Scoped auto-cleanup for event_base. */
#define raii_event_base [[gnu::cleanup(cleanup_event_base)]] struct event_base*
/** \brief Scoped auto-cleanup for evhttp. */
#define raii_evhttp [[gnu::cleanup(cleanup_evhttp)]] struct evhttp*
/** \brief Scoped auto-cleanup for json_tokener. */
#define raii_json_tokener [[gnu::cleanup(cleanup_json_tokener)]] struct json_tokener*
/** \brief Scoped auto-cleanup for json_object. */
#define raii_json_object [[gnu::cleanup(cleanup_json_object)]] struct json_object*

#include <stdio.h>
static inline void cleanup_file(FILE** fp) { if (*fp != nullptr) fclose(*fp); }
static inline void cleanup_free(void* p) { void** ptr = (void**)p; if (*ptr) free(*ptr); }

/** \brief Scoped auto-cleanup for FILE*. */
#define raii_file [[gnu::cleanup(cleanup_file)]] FILE*
/** \brief Scoped auto-cleanup for malloc'd pointers. */
#define raii_free [[gnu::cleanup(cleanup_free)]]

#include <sodium.h>
#include <string.h>
static inline void cleanup_secure_free_str(char** str) { 
    if (*str != nullptr) { 
        sodium_memzero(*str, strlen(*str)); 
        free(*str); 
    } 
}
/** \brief Scoped auto-cleanup for malloc'd null-terminated strings that require secure wiping. */
#define raii_secure_free_str [[gnu::cleanup(cleanup_secure_free_str)]] char*
