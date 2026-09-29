/*
 * user/tls/inc/string.h -- string.h minimal untuk mbedTLS bare-metal.
 * Implementasi di user/tls/tls_port.c.
 */
#ifndef QAON_STRING_H
#define QAON_STRING_H

void *memcpy(void *dst, const void *src, unsigned n);
void *memmove(void *dst, const void *src, unsigned n);
void *memset(void *dst, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
unsigned strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strstr(const char *h, const char *n);
char *strchr(const char *s, int c);

#endif /* QAON_STRING_H */
