/*
 * http.h - Server HTTP/1.0 minimal untuk system monitor, Fase 12.
 *
 * http_handle() menerima request mentah (sudah lengkap sampai \r\n\r\n)
 * dan membangun respons HTTP/1.0 lengkap di buffer resp.
 * Mengembalikan panjang respons (0 bila request tak valid -> abaikan).
 */
#ifndef MACH_HTTP_H
#define MACH_HTTP_H

#include <stdint.h>

unsigned http_handle(const uint8_t *req, unsigned reqlen, uint8_t *resp);

#endif /* MACH_HTTP_H */
