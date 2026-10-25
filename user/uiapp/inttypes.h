/*
 * inttypes.h - freestanding PRI* macros for the LVGL build (UI-1).
 *
 * Clang's resource inttypes.h does `#include_next` into a
 * libc header that does not exist in this freestanding
 * userspace build, so LVGL's <inttypes.h> include fails.
 * This file (found first via -I user/uiapp) supplies
 * only the format macros LVGL uses. ARM EABI: int and
 * long are 32-bit, long long is 64-bit, intptr_t is int.
 */
#ifndef UIAPP_INTTYPES_H
#define UIAPP_INTTYPES_H

#include <stdint.h>

#define PRId8		"d"
#define PRId16		"d"
#define PRId32		"d"
#define PRId64		"lld"
#define PRIdLEAST8	"d"
#define PRIdLEAST16	"d"
#define PRIdLEAST32	"d"
#define PRIdLEAST64	"lld"
#define PRIdFAST8	"d"
#define PRIdFAST16	"d"
#define PRIdFAST32	"d"
#define PRIdFAST64	"lld"
#define PRIdMAX		"lld"
#define PRIdPTR		"d"

#define PRIi8		"i"
#define PRIi16		"i"
#define PRIi32		"i"
#define PRIi64		"lli"
#define PRIiLEAST8	"i"
#define PRIiLEAST16	"i"
#define PRIiLEAST32	"i"
#define PRIiLEAST64	"lli"
#define PRIiFAST8	"i"
#define PRIiFAST16	"i"
#define PRIiFAST32	"i"
#define PRIiFAST64	"lli"
#define PRIiMAX		"lli"
#define PRIiPTR		"i"

#define PRIo8		"o"
#define PRIo16		"o"
#define PRIo32		"o"
#define PRIo64		"llo"
#define PRIoLEAST8	"o"
#define PRIoLEAST16	"o"
#define PRIoLEAST32	"o"
#define PRIoLEAST64	"llo"
#define PRIoFAST8	"o"
#define PRIoFAST16	"o"
#define PRIoFAST32	"o"
#define PRIoFAST64	"llo"
#define PRIoMAX		"llo"
#define PRIoPTR		"o"

#define PRIu8		"u"
#define PRIu16		"u"
#define PRIu32		"u"
#define PRIu64		"llu"
#define PRIuLEAST8	"u"
#define PRIuLEAST16	"u"
#define PRIuLEAST32	"u"
#define PRIuLEAST64	"llu"
#define PRIuFAST8	"u"
#define PRIuFAST16	"u"
#define PRIuFAST32	"u"
#define PRIuFAST64	"llu"
#define PRIuMAX		"llu"
#define PRIuPTR		"u"

#define PRIx8		"x"
#define PRIx16		"x"
#define PRIx32		"x"
#define PRIx64		"llx"
#define PRIxLEAST8	"x"
#define PRIxLEAST16	"x"
#define PRIxLEAST32	"x"
#define PRIxLEAST64	"llx"
#define PRIxFAST8	"x"
#define PRIxFAST16	"x"
#define PRIxFAST32	"x"
#define PRIxFAST64	"llx"
#define PRIxMAX		"llx"
#define PRIxPTR		"x"

#define PRIX8		"X"
#define PRIX16		"X"
#define PRIX32		"X"
#define PRIX64		"llX"
#define PRIXLEAST8	"X"
#define PRIXLEAST16	"X"
#define PRIXLEAST32	"X"
#define PRIXLEAST64	"llX"
#define PRIXFAST8	"X"
#define PRIXFAST16	"X"
#define PRIXFAST32	"X"
#define PRIXFAST64	"llX"
#define PRIXMAX		"llX"
#define PRIXPTR		"X"

#endif /* UIAPP_INTTYPES_H */
