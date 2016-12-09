#include <stdint.h>
#include <string.h>

void
memzero (void *p, uint32_t size)
{
    memset(p, 0, size);
    return;
}

