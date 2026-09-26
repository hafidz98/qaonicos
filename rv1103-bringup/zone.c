/* zone.c - fixed-size object cache over static memory. */
#include "zone.h"
#include <stdint.h>
#include <stddef.h>

void zone_init(struct zone *z, void *base, unsigned total, unsigned objsize)
{
    uint8_t *p = (uint8_t *)base;
    unsigned n, i;

    if (objsize < sizeof(void *))
        objsize = sizeof(void *);
    /* Align object size to pointer size for sane free-list links. */
    objsize = (objsize + sizeof(void *) - 1) & ~(sizeof(void *) - 1);

    z->objsize = objsize;
    z->free_list = NULL;
    n = total / objsize;
    for (i = 0; i < n; i++) {
        void **link = (void **)(p + i * objsize);
        *link = z->free_list;
        z->free_list = link;
    }
}

void *zalloc(struct zone *z)
{
    void *obj = z->free_list;
    if (obj)
        z->free_list = *(void **)obj;
    return obj;
}

void zfree(struct zone *z, void *obj)
{
    if (!obj)
        return;
    *(void **)obj = z->free_list;
    z->free_list = obj;
}
