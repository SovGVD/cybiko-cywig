#pragma once
/* Windows (MinGW) only, given to every file with -include: what the C
 * library there lacks (see win_compat.c). */
char *strcasestr(const char *haystack, const char *needle);
