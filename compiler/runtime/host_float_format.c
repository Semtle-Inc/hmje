#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Self-hosting compiler support for the reference format(value, ".16e")
 * contract. Let the host libc perform decimal-to-binary rounding. */
int64_t hmje_format_f64_literal_e16(uint64_t input_address, uint64_t input_length,
                                    uint64_t bit_width, uint64_t output_address,
                                    uint64_t output_capacity) {
    if (!input_address || !output_address || !input_length || input_length >= 128 ||
        !output_capacity) return -1;
    char input[128];
    memcpy(input, (const void *)(uintptr_t)input_address, (size_t)input_length);
    input[input_length] = '\0';
    errno = 0;
    char *end = NULL;
    double value = strtod(input, &end);
    if (end != input + input_length || errno == ERANGE) return -1;
    if (bit_width == 32) value = (double)(float)value;
    else if (bit_width != 64) return -1;
    int written = snprintf((char *)(uintptr_t)output_address,
                           (size_t)output_capacity, "%.16e", value);
    return written < 0 || (uint64_t)written >= output_capacity ? -1 : written;
}

int64_t hmje_format_float_binary_e16(uint64_t left_address, uint64_t left_length,
                                     uint64_t right_address, uint64_t right_length,
                                     uint64_t operation, uint64_t bit_width,
                                     uint64_t output_address, uint64_t output_capacity) {
    if (!left_address || !right_address || !output_address ||
        !left_length || left_length >= 128 || !right_length || right_length >= 128 ||
        !output_capacity) return -1;
    char left_text[128], right_text[128];
    memcpy(left_text, (const void *)(uintptr_t)left_address, (size_t)left_length);
    memcpy(right_text, (const void *)(uintptr_t)right_address, (size_t)right_length);
    left_text[left_length] = '\0';
    right_text[right_length] = '\0';
    char *left_end = NULL, *right_end = NULL;
    double left = strtod(left_text, &left_end);
    double right = strtod(right_text, &right_end);
    if (left_end != left_text + left_length || right_end != right_text + right_length)
        return -1;
    double result;
    if (bit_width == 32) {
        float a = (float)left, b = (float)right, value;
        if (operation == 0) value = a + b;
        else if (operation == 1) value = a - b;
        else if (operation == 2) value = a * b;
        else if (operation == 3 && b != 0.0f) value = a / b;
        else return -1;
        result = (double)value;
    } else if (bit_width == 64) {
        if (operation == 0) result = left + right;
        else if (operation == 1) result = left - right;
        else if (operation == 2) result = left * right;
        else if (operation == 3 && right != 0.0) result = left / right;
        else return -1;
    } else return -1;
    int written = snprintf((char *)(uintptr_t)output_address,
                           (size_t)output_capacity, "%.16e", result);
    return written < 0 || (uint64_t)written >= output_capacity ? -1 : written;
}
