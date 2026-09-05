#include <apiserver/json_util.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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

static const char digits_lut[200] = {
    '0','0', '0','1', '0','2', '0','3', '0','4', '0','5', '0','6', '0','7', '0','8', '0','9',
    '1','0', '1','1', '1','2', '1','3', '1','4', '1','5', '1','6', '1','7', '1','8', '1','9',
    '2','0', '2','1', '2','2', '2','3', '2','4', '2','5', '2','6', '2','7', '2','8', '2','9',
    '3','0', '3','1', '3','2', '3','3', '3','4', '3','5', '3','6', '3','7', '3','8', '3','9',
    '4','0', '4','1', '4','2', '4','3', '4','4', '4','5', '4','6', '4','7', '4','8', '4','9',
    '5','0', '5','1', '5','2', '5','3', '5','4', '5','5', '5','6', '5','7', '5','8', '5','9',
    '6','0', '6','1', '6','2', '6','3', '6','4', '6','5', '6','6', '6','7', '6','8', '6','9',
    '7','0', '7','1', '7','2', '7','3', '7','4', '7','5', '7','6', '7','7', '7','8', '7','9',
    '8','0', '8','1', '8','2', '8','3', '8','4', '8','5', '8','6', '8','7', '8','8', '8','9',
    '9','0', '9','1', '9','2', '9','3', '9','4', '9','5', '9','6', '9','7', '9','8', '9','9'
};

static inline unsigned int count_digits_u32(uint32_t val) {
    if (val < 10) return 1;
    if (val < 100) return 2;
    if (val < 1000) return 3;
    if (val < 10000) return 4;
    if (val < 100000) return 5;
    if (val < 1000000) return 6;
    if (val < 10000000) return 7;
    if (val < 100000000) return 8;
    if (val < 1000000000) return 9;
    return 10;
}

static inline unsigned int count_digits_u64(uint64_t val) {
    if (val < 1000000000ULL) return count_digits_u32((uint32_t)val);
    if (val < 10000000000ULL) return 10;
    if (val < 100000000000ULL) return 11;
    if (val < 1000000000000ULL) return 12;
    if (val < 10000000000000ULL) return 13;
    if (val < 100000000000000ULL) return 14;
    if (val < 1000000000000000ULL) return 15;
    if (val < 10000000000000000ULL) return 16;
    if (val < 100000000000000000ULL) return 17;
    if (val < 1000000000000000000ULL) return 18;
    if (val < 10000000000000000000ULL) return 19;
    return 20;
}

static inline void write2(char* dst, uint32_t val) {
    memcpy(dst, &digits_lut[val * 2], 2);
}

int fast_itoa(int val, char* buf, size_t buf_size) {
    if (!buf || buf_size < 2) return 0;
    uint32_t uval;
    bool neg = false;
    if (val < 0) {
        neg = true;
        uval = 0 - (uint32_t)val;
    } else {
        uval = (uint32_t)val;
    }

    unsigned int len = count_digits_u32(uval);
    if (neg) len++;

    if (len + 1 > buf_size) return 0;

    buf[len] = '\0';
    char* ptr = &buf[len];

    while (uval >= 10000) {
        uint32_t rem = uval % 10000;
        uval /= 10000;
        ptr -= 4;
        write2(ptr + 2, rem % 100);
        write2(ptr, rem / 100);
    }
    while (uval >= 100) {
        uint32_t rem = uval % 100;
        uval /= 100;
        ptr -= 2;
        write2(ptr, rem);
    }
    if (uval < 10) {
        *--ptr = (char)('0' + uval);
    } else {
        ptr -= 2;
        write2(ptr, uval);
    }

    if (neg) buf[0] = '-';
    return (int)len;
}

int fast_ltoa(long val, char* buf, size_t buf_size) {
    if (!buf || buf_size < 2) return 0;
    uint64_t uval;
    bool neg = false;
    if (val < 0) {
        neg = true;
        uval = 0 - (uint64_t)val;
    } else {
        uval = (uint64_t)val;
    }

    unsigned int len = count_digits_u64(uval);
    if (neg) len++;

    if (len + 1 > buf_size) return 0;

    buf[len] = '\0';
    char* ptr = &buf[len];

    while (uval >= 100000000) {
        uint64_t rem = uval % 100000000;
        uval /= 100000000;
        ptr -= 8;
        uint32_t r1 = rem % 10000;
        uint32_t r2 = rem / 10000;
        write2(ptr + 6, r1 % 100);
        write2(ptr + 4, r1 / 100);
        write2(ptr + 2, r2 % 100);
        write2(ptr, r2 / 100);
    }
    uint32_t uval32 = (uint32_t)uval;
    while (uval32 >= 10000) {
        uint32_t rem = uval32 % 10000;
        uval32 /= 10000;
        ptr -= 4;
        write2(ptr + 2, rem % 100);
        write2(ptr, rem / 100);
    }
    while (uval32 >= 100) {
        uint32_t rem = uval32 % 100;
        uval32 /= 100;
        ptr -= 2;
        write2(ptr, rem);
    }
    if (uval32 < 10) {
        *--ptr = (char)('0' + uval32);
    } else {
        ptr -= 2;
        write2(ptr, uval32);
    }

    if (neg) buf[0] = '-';
    return (int)len;
}


static inline void format_int_part(uint64_t temp_int, char* buf, unsigned int pos) {
    while (temp_int >= 100) {
        unsigned int rem = temp_int % 100;
        temp_int /= 100;
        pos -= 2;
        buf[pos] = digits_lut[rem * 2];
        buf[pos + 1] = digits_lut[rem * 2 + 1];
    }
    if (temp_int < 10) {
        buf[pos - 1] = (char)('0' + temp_int);
    } else {
        pos -= 2;
        buf[pos] = digits_lut[temp_int * 2];
        buf[pos + 1] = digits_lut[temp_int * 2 + 1];
    }
}

static inline unsigned int format_frac_part(double frac, char* buf, unsigned int pos, size_t buf_size, unsigned int len) {
    if (frac > 1e-9 && pos + 8 < buf_size) {
        buf[pos++] = '.';
        uint32_t frac_digits = (uint32_t)(frac * 1000000.0 + 0.5);

        for (int i = 5; i >= 1; i -= 2) {
            unsigned int rem = frac_digits % 100;
            frac_digits /= 100;
            buf[pos + i - 1] = digits_lut[rem * 2];
            buf[pos + i] = digits_lut[rem * 2 + 1];
        }
        pos += 6;
        
        while (pos > len && buf[pos - 1] == '0') {
            pos--;
        }
        if (pos > len && buf[pos - 1] == '.') {
            pos--;
        }
    }
    return pos;
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

    unsigned int len = count_digits_u64(int_part);
    if (neg) len++;

    if (len + 1 > buf_size) return 0;

    unsigned int pos = len;
    format_int_part(int_part, buf, pos);
    
    if (neg) buf[0] = '-';

    pos = format_frac_part(frac, buf, len, buf_size, len);
    
    if (pos == 1 && buf[0] == '-') {
        buf[0] = '0';
    }

    buf[pos] = '\0';
    return (int)pos;
}
