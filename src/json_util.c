#include "json_util.h"
#include <stdio.h>

void json_encode_string(const char* src, char* dest, size_t dest_size) {
    if (!src || !dest || dest_size == 0) return;
    
    size_t i = 0;
    size_t j = 0;
    while (src[i] && j < dest_size - 7) { 
        switch (src[i]) {
            case '"':  dest[j++] = '\\'; dest[j++] = '"'; break;
            case '\\': dest[j++] = '\\'; dest[j++] = '\\'; break;
            case '\n': dest[j++] = '\\'; dest[j++] = 'n'; break;
            case '\r': dest[j++] = '\\'; dest[j++] = 'r'; break;
            case '\t': dest[j++] = '\\'; dest[j++] = 't'; break;
            default:
                if ((unsigned char)src[i] < 0x20) {
                    int written = snprintf(&dest[j], dest_size - j, "\\u%04x", (unsigned char)src[i]);
                    if (written > 0 && written < (int)(dest_size - j)) {
                        j += (size_t)written;
                    }
                } else {
                    dest[j++] = src[i];
                }
                break;
        }
        i++;
    }
    dest[j] = '\0';
}
