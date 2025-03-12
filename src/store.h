#ifndef CM_STORE_H
#define CM_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "util.h"

#define CS_SNIP_SIZE 256         /* The size of each struct cs_snip */
#define CS_SNIP_ALLOC_BATCH 1024 /* How many snips to allocate when growing */
#define CS_HASH_STR_MAX 17       /* String length of 64bit hex + \0 */
#define PRI_HASH "%016" PRIX64

/**
 * A single snip within the clip store.
 *
 * @hash: A 64-bit hash value associated with the content entry
 * @doomed: Used during cs_remove to batch mark entries for removal
 * @nr_lines: The number of lines in the content entry
 * @line: A character array containing the first salient line, terminated by a
 *        null byte
 */
#define CS_SNIP_LINE_SIZE CS_SNIP_SIZE - (sizeof(uint64_t) * 2) - sizeof(bool)
struct _packed_ cs_snip {
    uint64_t hash;
    bool doomed;
    uint64_t nr_lines;
    char line[CS_SNIP_LINE_SIZE];
};

/**
 * The header of the clip store. Must fit within the footprint of a regular
 * `cs_snip`.
 *
 * @nr_snips: The total number of snips in the clip store, excluding the
 *              header
 * @nr_snips_alloc: The total number of allocated snips in the clip store
 *                    that can be used without _cs_file_resize(), excluding the
 *                    header
 * @_unused_padding: Padding to match the size of cs_snip
 */
#define CS_HEADER_PADDING_SIZE CS_SNIP_SIZE - (sizeof(uint64_t) * 2)
struct _packed_ cs_header {
    uint64_t nr_snips;
    uint64_t nr_snips_alloc;
    char _unused_padding[CS_HEADER_PADDING_SIZE];
};

static_assert(sizeof(struct cs_snip) == CS_SNIP_SIZE, "cs_snip wrong size");
static_assert(sizeof(struct cs_snip) == sizeof(struct cs_header),
              "cs_header and cs_snip must be the same size");

/**
 * The main interface to the clip store for the user.
 *
 * @snip_fd: The file descriptor for the snip file
 * @content_dir_fd: The file descriptor for the content directory
 * @header: Pointer to the header in the mmapped file
 * @snips: Pointer to the beginning of the clip store snips in the mmapped
 *           file, directly after the header
 * @ready: Indicates if the clip store is ready for operations
 * @refcount: The reference count for the fd flock
 * @local_nr_snips: Our last known header->nr_snips
 * @local_nr_snips_alloc: Our last known header->nr_snips_alloc
 */
struct clip_store {
    /* FDs */
    int snip_fd;
    int content_dir_fd;

    /* Pointers inside mmapped snip file */
    struct cs_header *header;
    struct cs_snip *snips;

    /* Synchronisation */
    size_t refcount;
    size_t local_nr_snips;
    size_t local_nr_snips_alloc;
    bool ready;
};

/**
 * Manages the lifecycle of a single clip store lock reference.
 *
 * @status: The result of trying update our mappings to new header values. If
 *          less than 0, the lock should not be used. Set to -EBADF on unref.
 * @unref: The function to call to unref. This mostly exists to ensure that
 *          the ref_guard is used and is not optimised by the compiler.
 * @cs: A pointer to the clip store structure on which the lock is held.
 *
 * Usage of this structure involves creating a ref guard instance at the
 * beginning of a scope where a lock is needed. Functions which need a lock
 * accept a `struct ref_guard *` as their first argument
 */
struct ref_guard {
    int status;
    struct clip_store *cs;
    void (*unref)(struct clip_store *);
};

/**
 * The memory-mapped content associated with a hash in the content directory.
 *
 * @data: A pointer to the memory-mapped data
 * @fd: The file descriptor of the opened file from which the content is mapped
 * @size: The size of the mapped data
 */
struct cs_content {
    char *data;
    int fd;
    off_t size;
};

/**
 * The direction in which to iterate over snips in the clip store.
 *
 * @CS_ITER_NEWEST_FIRST: Iterate over the snips starting from the newest.
 * @CS_ITER_OLDEST_FIRST: Iterate over the snips starting from the oldest.
 */
enum cs_iter_direction { CS_ITER_NEWEST_FIRST, CS_ITER_OLDEST_FIRST };

/**
 * Set the bit at position n.
 *
 * @n: The position of the bit to set
 */
#define BIT(n) (1UL << (n))

/**
 * The action to take as returned by `cs_remove`'s `should_remove()` predicate
 * function.
 *
 * @CS_ACTION_REMOVE: Remove this snip from the clip store
 * @CS_ACTION_KEEP: Keep this snip in the clip store
 * @CS_ACTION_STOP: Stop iteration after processing this snip
 *
 * If neither of CS_ACTION_REMOVE or CS_ACTION_KEEP are specified, the snip is
 * kept.
 */
enum cs_remove_action {
    CS_ACTION_REMOVE = BIT(0),
    CS_ACTION_KEEP = BIT(1),
    CS_ACTION_STOP = BIT(2),
};

/**
 * What to do when there's a duplicate entry.
 *
 * @CS_DUPE_KEEP_ALL: Keep all duplicate entries.
 * @CS_DUPE_KEEP_LAST: Only keep the newest, do not insert duplicate entries.
 */
enum cs_dupe_policy {
    CS_DUPE_KEEP_ALL,
    CS_DUPE_KEEP_LAST,
};

struct ref_guard _must_use_ _nonnull_ cs_ref(struct clip_store *cs);
void _nonnull_ cs_unref(struct clip_store *cs);
void _nonnull_ drop_cs_unref(struct ref_guard *guard);
int _must_use_ _nonnull_ cs_destroy(struct clip_store *cs);
int _must_use_ _nonnull_ cs_init(struct clip_store *cs, int snip_fd,
                                 int content_dir_fd);
int _must_use_ cs_content_unmap(struct cs_content *content);
void drop_cs_content_unmap(struct cs_content *content);
void drop_cs_destroy(struct clip_store *cs);
int _must_use_ _nonnull_ cs_content_get(struct clip_store *cs, uint64_t hash,
                                        struct cs_content *content);
int _must_use_ _nonnull_n_(1)
    cs_make_newest(struct clip_store *cs, uint64_t hash);
int _must_use_ _nonnull_n_(1)
    cs_add(struct clip_store *cs, const char *content, uint64_t *out_hash,
           enum cs_dupe_policy dupe_policy);
bool _must_use_ _nonnull_ cs_snip_iter(struct ref_guard *guard,
                                       enum cs_iter_direction direction,
                                       struct cs_snip **snip);
int _must_use_ _nonnull_ cs_remove(
    struct clip_store *cs, enum cs_iter_direction direction,
    enum cs_remove_action (*should_remove)(uint64_t, const char *, void *),
    void *private);
int _must_use_ _nonnull_ cs_trim(struct clip_store *cs,
                                 enum cs_iter_direction direction,
                                 size_t nr_keep);
int _must_use_ _nonnull_n_(1, 4)
    cs_replace(struct clip_store *cs, enum cs_iter_direction direction,
               size_t age, const char *content, uint64_t *out_hash);
int _nonnull_ cs_len(struct clip_store *cs, size_t *out_len);

size_t _nonnull_ first_line(const char *text, char *out);

#endif
