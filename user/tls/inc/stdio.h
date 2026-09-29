/*
 * user/tls/inc/stdio.h -- stdio.h minimal untuk mbedTLS bare-metal.
 * snprintf/printf/vsnprintf diimplementasi di user/tls/tls_port.c.
 */
#ifndef QAON_STDIO_H
#define QAON_STDIO_H

#include <stdarg.h>

int snprintf(char *out, unsigned outlen, const char *fmt, ...);
int vsnprintf(char *out, unsigned outlen, const char *fmt, va_list ap);
int printf(const char *fmt, ...);

#endif /* QAON_STDIO_H */
