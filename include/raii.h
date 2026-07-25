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

/** \brief Scoped auto-cleanup for event_config. */
#define raii_event_config [[gnu::cleanup(cleanup_event_config)]] struct event_config*
/** \brief Scoped auto-cleanup for event_base. */
#define raii_event_base [[gnu::cleanup(cleanup_event_base)]] struct event_base*
/** \brief Scoped auto-cleanup for evhttp. */
#define raii_evhttp [[gnu::cleanup(cleanup_evhttp)]] struct evhttp*
/** \brief Scoped auto-cleanup for json_tokener. */
#define raii_json_tokener [[gnu::cleanup(cleanup_json_tokener)]] struct json_tokener*
