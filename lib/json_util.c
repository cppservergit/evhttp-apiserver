#include <apiserver/json_util.h>
#include <stdio.h>

static const char hex[] = "0123456789abcdef";

void json_encode_string(const char* src, char* dest, size_t dest_size) {
    if (!src || !dest || dest_size == 0) return;
    
    size_t i = 0;
    size_t j = 0;
    while (src[i] && j + 7 < dest_size) { 
        switch (src[i]) {
            case '"':  dest[j++] = '\\'; dest[j++] = '"'; break;
            case '\\': dest[j++] = '\\'; dest[j++] = '\\'; break;
            case '\n': dest[j++] = '\\'; dest[j++] = 'n'; break;
            case '\r': dest[j++] = '\\'; dest[j++] = 'r'; break;
            case '\t': dest[j++] = '\\'; dest[j++] = 't'; break;
            case '\b': dest[j++] = '\\'; dest[j++] = 'b'; break;
            case '\f': dest[j++] = '\\'; dest[j++] = 'f'; break;
            default: {
                unsigned char c = (unsigned char)src[i];
                if (c >= 0x20) {
                    dest[j++] = (char)c;
                    break;
                }
                
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

#include <math.h>
#include <stdint.h>
#include <stdbool.h>

int fast_itoa(int val, char* buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    char temp[12];
    int p = 0;
    unsigned int uval;
    bool neg = false;
    if (val < 0) {
        neg = true;
        uval = 0 - (unsigned int)val;
    } else {
        uval = (unsigned int)val;
    }
    while (uval > 0) {
        temp[p++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    int out_p = 0;
    if (neg) buf[out_p++] = '-';
    while (p > 0) {
        buf[out_p++] = temp[--p];
    }
    buf[out_p] = '\0';
    return out_p;
}

int fast_ltoa(long val, char* buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    char temp[24];
    int p = 0;
    unsigned long uval;
    bool neg = false;
    if (val < 0) {
        neg = true;
        uval = 0 - (unsigned long)val;
    } else {
        uval = (unsigned long)val;
    }
    while (uval > 0) {
        temp[p++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    int out_p = 0;
    if (neg) buf[out_p++] = '-';
    while (p > 0) {
        buf[out_p++] = temp[--p];
    }
    buf[out_p] = '\0';
    return out_p;
}

int fast_dtoa(double val, char* buf, size_t buf_size) {
    if (!buf || buf_size < 2) return 0;

    if (isnan(val) || isinf(val)) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    if (val > 1e15 || val < -1e15) {
        int w = snprintf(buf, buf_size, "%.17g", val);
        return (w > 0 && (size_t)w < buf_size) ? w : 0;
    }

    bool neg = signbit(val);
    double abs_val = neg ? -val : val;

    uint64_t int_part = (uint64_t)abs_val;
    double frac = abs_val - (double)int_part;

    char temp[24];
    int tp = 0;
    if (int_part == 0) {
        temp[tp++] = '0';
    } else {
        while (int_part > 0) {
            temp[tp++] = (char)('0' + (int_part % 10));
            int_part /= 10;
        }
    }

    size_t p = 0;
    if (neg && p + 1 < buf_size) {
        buf[p++] = '-';
    }

    while (tp > 0 && p + 1 < buf_size) {
        buf[p++] = temp[--tp];
    }

    if (frac > 1e-9 && p + 2 < buf_size) {
        buf[p++] = '.';
        static const double POW10[] = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0};
        uint64_t frac_digits = (uint64_t)(frac * POW10[6] + 0.5);

        char frac_temp[6];
        for (int i = 5; i >= 0; --i) {
            frac_temp[i] = (char)('0' + (frac_digits % 10));
            frac_digits /= 10;
        }

        for (int i = 0; i < 6 && p + 1 < buf_size; ++i) {
            buf[p++] = frac_temp[i];
        }

        while (p > 0 && buf[p - 1] == '0') {
            p--;
        }
        if (p > 0 && buf[p - 1] == '.') {
            p--;
        }
    }

    if (p == 1 && buf[0] == '-') {
        buf[0] = '0';
    }

    buf[p] = '\0';
    return (int)p;
}
