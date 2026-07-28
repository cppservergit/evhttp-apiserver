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
            default: {
                unsigned char c = (unsigned char)src[i];
                if (c >= 0x20) {
                    dest[j++] = (char)c;
                    break;
                }
                
                const char hex[] = "0123456789abcdef";
                dest[j++] = '\\';
                dest[j++] = 'u';
                dest[j++] = '0';
                dest[j++] = '0';
                dest[j++] = hex[c >> 4];
                dest[j++] = hex[c & 0x0F];
                break;
            }
        }
        i++;
    }
    dest[j] = '\0';
}
