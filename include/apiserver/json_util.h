#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stddef.h>

/**
 * \file json_util.h
 * \brief JSON string encoding utilities.
 */

#define JSON_ENCODE_BUFSIZE(src_max) (((src_max) * 6) + 1)

/**
 * \brief Encodes/escapes a plaintext string into a JSON-safe string format.
 * 
 * Fully stack-based and zero-allocation.
 * 
 * \param src The source null-terminated string to encode.
 * \param dest The destination buffer to write the encoded string into.
 * \param dest_size The total allocated size of the destination buffer.
 */
void json_encode_string(const char* src, char* dest, size_t dest_size);

#endif // JSON_UTIL_H
