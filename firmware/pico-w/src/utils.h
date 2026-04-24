#ifndef WAXWING_UTILS_H
#define WAXWING_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Delay functions (would use Pico SDK sleep_ms/sleep_us in real implementation)
void waxwing_utils_sleep_ms(uint32_t ms);

// Time functions
uint32_t waxwing_utils_get_time_ms(void);
time_t waxwing_utils_get_time_sec(void); // Seconds since epoch

// String utilities
int waxwing_utils_strncmp(const char *s1, const char *s2, size_t n);
char *waxwing_utils_strncpy(char *dest, const char *src, size_t n);

// Conversion utilities
uint8_t waxwing_utils_hex_char_to_value(char c);
bool waxwing_utils_is_hex_string(const char *str);

#endif // WAXWING_UTILS_H