/* Minimal stdint shim for freestanding kernel build. */
#ifndef QAONIC_SHIM_STDINT_H
#define QAONIC_SHIM_STDINT_H
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int uintptr_t;
typedef int intptr_t;
typedef unsigned int size_t;
#define UINT32_MAX 0xffffffffu
#define INT32_MAX 0x7fffffff
#define INT32_MIN (-2147483647 - 1)
#endif
