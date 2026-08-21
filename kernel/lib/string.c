#include <string.h>
#include <stdarg.h>

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++)) {}
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    return n ? *(const unsigned char *)s1 - *(const unsigned char *)s2 : 0;
}

static void print_num(char **buf, size_t *size, uint64_t num, int base, int width, char pad_char) {
    char tmp[32];
    int i = 0;
    
    if (num == 0) {
        tmp[i++] = '0';
    } else {
        while (num > 0) {
            int digit = num % base;
            tmp[i++] = (digit < 10) ? '0' + digit : 'a' + digit - 10;
            num /= base;
        }
    }
    
    while (i < width) {
        tmp[i++] = pad_char;
    }
    
    while (i > 0 && *size > 1) {
        *(*buf)++ = tmp[--i];
        (*size)--;
    }
}

static void print_signed_num(char **buf, size_t *size, int64_t num, int base, int width, char pad_char) {
    if (num < 0 && base == 10) {
        if (*size > 1) {
            *(*buf)++ = '-';
            (*size)--;
        }
        num = -num;
    }
    print_num(buf, size, (uint64_t)num, base, width, pad_char);
}

static int do_vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    char *buf = str;
    size_t remaining = size;
    
    if (size == 0)
        return 0;
    
    while (*format && remaining > 1) {
        if (*format == '%') {
            format++;
            int width = 0;
            char pad_char = ' ';
            
            if (*format == '0') {
                pad_char = '0';
                format++;
            }
            
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }
            
            switch (*format) {
                case 'd':
                case 'i':
                    print_signed_num(&buf, &remaining, va_arg(ap, int), 10, width, pad_char);
                    break;
                case 'u':
                    print_num(&buf, &remaining, va_arg(ap, unsigned int), 10, width, pad_char);
                    break;
                case 'x':
                case 'X':
                    print_num(&buf, &remaining, va_arg(ap, unsigned int), 16, width, pad_char);
                    break;
                case 'p':
                    if (remaining > 1) {
                        *buf++ = '0';
                        remaining--;
                    }
                    if (remaining > 1) {
                        *buf++ = 'x';
                        remaining--;
                    }
                    print_num(&buf, &remaining, (uintptr_t)va_arg(ap, void *), 16, 2 * sizeof(void *), '0');
                    break;
                case 'l':
                    format++;
                    if (*format == 'l') {
                        format++;
                        if (*format == 'u') {
                            print_num(&buf, &remaining, va_arg(ap, unsigned long long), 10, width, pad_char);
                        } else if (*format == 'd' || *format == 'i') {
                            print_signed_num(&buf, &remaining, va_arg(ap, long long), 10, width, pad_char);
                        } else if (*format == 'x' || *format == 'X') {
                            print_num(&buf, &remaining, va_arg(ap, unsigned long long), 16, width, pad_char);
                        }
                    } else if (*format == 'u') {
                        print_num(&buf, &remaining, va_arg(ap, unsigned long), 10, width, pad_char);
                    } else if (*format == 'd' || *format == 'i') {
                        print_signed_num(&buf, &remaining, va_arg(ap, long), 10, width, pad_char);
                    } else if (*format == 'x' || *format == 'X') {
                        print_num(&buf, &remaining, va_arg(ap, unsigned long), 16, width, pad_char);
                    }
                    break;
                case 's': {
                    const char *s = va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    while (*s && remaining > 1) {
                        *buf++ = *s++;
                        remaining--;
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(ap, int);
                    if (remaining > 1) {
                        *buf++ = c;
                        remaining--;
                    }
                    break;
                }
                case '%':
                    if (remaining > 1) {
                        *buf++ = '%';
                        remaining--;
                    }
                    break;
                default:
                    if (remaining > 1) {
                        *buf++ = '%';
                        remaining--;
                    }
                    if (remaining > 1) {
                        *buf++ = *format;
                        remaining--;
                    }
                    break;
            }
        } else {
            *buf++ = *format;
            remaining--;
        }
        format++;
    }
    
    *buf = '\0';
    return buf - str;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    return do_vsnprintf(str, size, format, ap);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = do_vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}