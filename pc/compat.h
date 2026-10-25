#pragma once
/* What POSIX has and Windows (MinGW) doesn't, for cywig. */
#include <stddef.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
/* getentropy(): 0 when buf is filled with random bytes */
static inline int getentropy(void *buf, size_t n)
{
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}
#else
#include <sys/random.h>
#endif
