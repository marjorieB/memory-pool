#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>

#include "common.h"
#include "mpool.h"

#define GUARD 0x4242

struct memhdr {
	uint16_t guard;
	uint16_t length;
} PACKED;

extern void * __libc_malloc(size_t size);
extern void   __libc_free(void *ptr);
extern void * __libc_calloc(size_t nmemb, size_t size);
extern void * __libc_realloc(void *ptr, size_t size);

struct alloc_stats {
	long num_mpool_alloc;
	long num_mpool_free;
	long num_mpool_realloc;
	long num_sys_alloc;
	long num_sys_free;
	long num_sys_realloc;
};
static struct alloc_stats stats = {0};

static void * arena;
static size_t arena_size;
static bool alloc_overload_done = 0;
static void __attribute__((constructor))
alloc_overload_init(void)
{

	int rv;
	unsigned int weights[] = {10, 1, 1, 1, 1, 1, 1};

	arena_size = 1 << 24;  /* 16 MBytes */
	arena = mmap(NULL, arena_size,
			PROT_READ|PROT_WRITE,
			MAP_ANONYMOUS|MAP_SHARED,
			-1, 0);

	rv = mpool_create(arena, arena_size, weights, arraylen(weights));	
	if (rv != 0) {
		fprintf(stderr, "mpool_create() failed\n");
		exit(EXIT_FAILURE);
	}

	alloc_overload_done = 1;
}

static void __attribute__((destructor))
alloc_overload_cleanup(void)
{
	mpool_stats();
	printf("\n");
	printf("mpool_alloc:   %ld\n", stats.num_mpool_alloc);
	printf("mpool_free:    %ld\n", stats.num_mpool_free);
	printf("mpool_realloc: %ld\n", stats.num_mpool_realloc);
	printf("sys alloc:     %ld\n", stats.num_sys_alloc);
	printf("sys free:      %ld\n", stats.num_sys_free);
	printf("sys realloc:   %ld\n", stats.num_sys_realloc);

	fflush(stdout);
	fflush(stderr);

	mpool_destroy();
	munmap(arena, arena_size);
}

extern void * malloc (size_t size)
{
	struct memhdr * result;

	if (unlikely(!alloc_overload_done))
		return __libc_malloc(size);

	size += sizeof(struct memhdr);

	result = mpool_alloc(size, 0);
	if (result == NULL) {
		stats.num_sys_alloc++;
		return __libc_malloc(size);
	}

	*result = (struct memhdr) {
		.guard = GUARD,
		.length = (uint16_t) size,
	};

	stats.num_mpool_alloc++;
	return result + 1;
}

extern void free (void *ptr)
{
	struct memhdr * hdr;

	if (unlikely(!alloc_overload_done)) {
		__libc_free(ptr);
		return;
	}

	if (ptr == NULL)
		return;

	hdr = (struct memhdr *) ptr - 1;
	if (unlikely(hdr->guard != GUARD)) {
		stats.num_sys_free++;
		__libc_free(ptr);
		return;
	}

	stats.num_mpool_free++;
	mpool_free (hdr, hdr->length);
}

extern void * realloc(void *ptr, size_t size)
{
	uint16_t old_length;
	struct memhdr * hdr;

	if (unlikely(!alloc_overload_done))
		return __libc_realloc(ptr, size);

	if (ptr == NULL)
		return malloc(size);

	hdr = (struct memhdr *) ptr - 1;
	if (unlikely(hdr->guard != GUARD)) {
		stats.num_sys_realloc++;
		printf("%s:%d - sys_realloc(%p, %zu)\n", __func__, __LINE__, ptr, size);
		return __libc_realloc(ptr, size);
	}

	old_length = hdr->length;
	hdr->guard = 0x0000;
	size += sizeof(*hdr);

	hdr = mpool_realloc(hdr, old_length, size, 0);
	if (hdr == NULL) {
		mpool_free(ptr, old_length);
		return malloc(size);
	}

	stats.num_mpool_realloc++;
	*hdr = (struct memhdr) {
		.guard = GUARD,
		.length = (uint16_t) size,
	};

	return hdr + 1;
}

extern void* calloc(size_t nmemb, size_t size)
{
	void* ptr;

	if (unlikely(!alloc_overload_done))
		return __libc_calloc(nmemb, size);

	size = size * nmemb;

	if (size == 0)
		return NULL;

	ptr = malloc(size);
	if (ptr != NULL)
		memset(ptr, 0, size);

	return ptr;
}
