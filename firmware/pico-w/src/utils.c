#include "utils.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

void waxwing_utils_sleep_ms(uint32_t ms) {
    sleep_ms(ms);
}

uint32_t waxwing_utils_get_time_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

time_t waxwing_utils_get_time_sec(void) {
    return (time_t)(to_ms_since_boot(get_absolute_time()) / 1000);
}

int waxwing_utils_strncmp(const char *s1, const char *s2, size_t n) {
    return strncmp(s1, s2, n);
}

char *waxwing_utils_strncpy(char *dest, const char *src, size_t n) {
    return strncpy(dest, src, n);
}

uint8_t waxwing_utils_hex_char_to_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

bool waxwing_utils_is_hex_string(const char *str) {
    if (!str) return false;
    while (*str) {
        if (!((*str >= '0' && *str <= '9') ||
               (*str >= 'a' && *str <= 'f') ||
               (*str >= 'A' && *str <= 'F'))) {
            return false;
        }
        str++;
    }
    return true;
}
