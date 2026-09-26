/*
 * zone.h - Mach-style zone allocator (bring-up subset).
 *
 * A zone is a cache of fixed-size objects carved out of a static memory
 * region, managed as a singly-linked free list. No locking: single CPU,
 * cooperative scheduling at this stage. This is the kernel's malloc for
 * ports and messages.
 */
#ifndef _ZONE_H_
#define _ZONE_H_

struct zone {
    void *free_list;
    unsigned objsize;
};

/* Carve [base, base+total) into objsize chunks. objsize must be >= 4. */
void zone_init(struct zone *z, void *base, unsigned total, unsigned objsize);

/* Returns NULL when empty. */
void *zalloc(struct zone *z);
void zfree(struct zone *z, void *obj);

#endif /* _ZONE_H_ */
