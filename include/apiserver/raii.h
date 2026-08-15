#pragma once

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <stdio.h>
#include <sodium.h>
#include <string.h>
/**
 * \file raii.h
 * \brief Resource Acquisition Is Initialization (RAII) memory cleanup macros using GCC's __attribute__((cleanup)).
 */

static inline void cleanup_event_config(struct event_config** cfg) { if (*cfg != nullptr) event_config_free(*cfg); }
static inline void cleanup_event_base(struct event_base** base) { if (*base != nullptr) event_base_free(*base); }
static inline void cleanup_evhttp(const struct evhttp** http) { if (*http != nullptr) evhttp_free((struct evhttp*)(uintptr_t)*http); }
static inline void cleanup_json_tokener(struct json_tokener** tok) { if (*tok != nullptr) json_tokener_free(*tok); }
static inline void cleanup_json_object(struct json_object** obj) { if (*obj != nullptr) json_object_put(*obj); }


static inline void cleanup_file(FILE** fp) { if (*fp != nullptr) fclose(*fp); }
static inline void cleanup_free(void* p) { void** ptr = (void**)p; if (*ptr) free(*ptr); }


static inline void cleanup_secure_free_str(char** str) { 
    if (*str != nullptr) { 
        sodium_memzero(*str, strlen(*str)); 
        free(*str); 
    } 
}

