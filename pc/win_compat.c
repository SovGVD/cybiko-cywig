/* Windows (MinGW) only: what its C library lacks, see win_compat.h. */
#include <ctype.h>
#include <stddef.h>
#include "win_compat.h"

char *strcasestr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        size_t i = 0;
        while (needle[i] && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i])) i++;
        if (!needle[i]) return (char *)haystack;
    }
    return NULL;
}
