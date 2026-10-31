/* strlcpy/strlcat for glibc < 2.38 (Debian bookworm). BSD semantics. */
#include <string.h>

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dlen = strnlen(dst, size);
    if (dlen == size)
        return size + strlen(src);
    return dlen + strlcpy(dst + dlen, src, size - dlen);
}
