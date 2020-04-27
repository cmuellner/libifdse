#ifndef HELPERS_H_
#define HELPERS_H_

#include <inttypes.h>
#include <string.h>
#include <stddef.h>

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

static inline int starts_with(const char *pre, const char *str)
{
    size_t lenpre = strlen(pre);
    size_t lenstr = strlen(str);

    return lenstr < lenpre ? 0 : memcmp(pre, str, lenpre) == 0;
}

static inline uint16_t swap_uint16(uint16_t v)
{
	return (v << 8) | (v >> 8);
}

#endif /* HELPERS_H_ */
