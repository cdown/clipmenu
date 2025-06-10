#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "store.h"

/**
 * TERMINOLOGY
 *
 * - Clip store: A pairing of a file containing snips and a content directory
 * - Clip store entry: A single pair of snip and content entry
 * - Snip file: A file which maps lines to content entries using hashes
 * - Snip: A (hash, line) pair in the clip store, representing a content entry
 * - Content entry: The full data stored from the clipboard
 * - Content directory: The directory in which the content entries are stored
 *
 * KEY FUNCTIONS:
 *
 * - cs_add - add a clip store entry
 * - cs_remove - remove a clip store entry by callback
 * - cs_trim - trim to the newest/oldest N entries
 * - cs_snip_iter - iterate over snip hashes and lines
 * - cs_content_get - get the content for a snip hash
 *
 * CLIP STORE DESIGN
 *
 * Our primary focus is on achieving high efficiency in appending new snips,
 * iterating over the entire list of snips, and replacing the final snip in the
 * snip file. For this reason complexity is avoided in processing deletions,
 * since deletions are usually rare. This avoids us having to implement (for
 * example) tombstones, which would slow things down and complicate iteration.
 *
 * CONTENT DIRECTORY DESIGN
 *
 * The content directory is extremely simple: it contains files with the same
 * name as the snip hash. This allows quickly going from the one line summary
 * in the snip to the full contents.
 *
 * SYNCHRONISATION
 *
 * The clip store's size may be increased or decreased by another program using
 * the library, so before each operation we must take a lock and check if the
 * header was updated. If it was, we update the size of the mmapped area to
 * suit. The lock is implemented using flock() on cs->snip_fd, see cs_ref(),
 * cs_ref_no_update(), and cs_unref().
 */

/**
 * Calculate the needed file size in bytes for @nr_snips snips, adding the
 * header.
 *
 * @nr_snips: The number of snips to calculate for
 */
static size_t _must_use_ cs_file_size(size_t nr_snips) {
    return (nr_snips + 1) * CS_SNIP_SIZE;
}

/**
 * Validate the consistency of the clip store's header information.
 *
 * @cs: The clip store to operate on
 * @file_size: The current size of the file
 */
static int _must_use_ _nonnull_ cs_header_validate(const struct clip_store *cs,
                                                   size_t file_size) {
    if (cs->header->nr_snips > cs->header->nr_snips_alloc ||
        (cs->header->nr_snips_alloc + 1) * CS_SNIP_SIZE != file_size) {
        return -EINVAL;
    }
    return 0;
}

/**
 * Decrease the reference count for the clip store lock, unrefing it if
 * the refcount reaches zero.
 *
 * @cs: The clip store to operate on
 */
void cs_unref(struct clip_store *cs) {
    expect(cs->refcount > 0);
    cs->refcount--;
    if (cs->refcount == 0) {
        expect(flock(cs->snip_fd, LOCK_UN) == 0);
    }
}

/**
 * Increase the reference count for the clip store lock.
 *
 * @cs: The clip store to operate on
 */
static struct ref_guard _must_use_ _nonnull_
cs_ref_no_update(struct clip_store *cs) {
    struct ref_guard guard = {.status = 0, .unref = cs_unref, .cs = cs};
    if (cs->refcount == 0) {
        expect(flock(cs->snip_fd, LOCK_EX) == 0);
    }
    static_assert(sizeof(cs->refcount) == sizeof(size_t),
                  "refcount type wrong");
    expect(cs->refcount < SIZE_MAX);
    cs->refcount++;
    return guard;
}

/**
 * Increase the reference count for the clip store lock, and remap as needed if
 * the header values have changed.
 *
 * Even if the guard status indicates an error, you must still call cs_unref().
 *
 * @cs: The clip store to operate on
 */
struct ref_guard cs_ref(struct clip_store *cs) {
    struct ref_guard guard = cs_ref_no_update(cs);

    if (cs->refcount > 1) {
        // We're an inner reference, so any necessary remapping has already
        // been performed.
        return guard;
    }

    if (cs->local_nr_snips != cs->header->nr_snips ||
        cs->local_nr_snips_alloc != cs->header->nr_snips_alloc) {
        struct stat st;
        if (fstat(cs->snip_fd, &st) < 0) {
            guard.status = negative_errno();
            return guard;
        }

        int ret = cs_header_validate(cs, (size_t)st.st_size);
        if (ret < 0) {
            guard.status = ret;
            return guard;
        }

        // If we shrank, no need to remap, since we'll just use the new bounds.
        if (cs->local_nr_snips_alloc < cs->header->nr_snips_alloc) {
            struct cs_header *new_header = mremap(
                cs->header, cs_file_size(cs->local_nr_snips_alloc),
                cs_file_size(cs->header->nr_snips_alloc), MREMAP_MAYMOVE);
            if (new_header == MAP_FAILED) {
                guard.status = negative_errno();
                return guard;
            }

            cs->header = new_header;
            cs->snips = (struct cs_snip *)(cs->header + 1);
        }

        cs->local_nr_snips = cs->header->nr_snips;
        cs->local_nr_snips_alloc = cs->header->nr_snips_alloc;
    }

    return guard;
}

/**
 * _drop_() function for when a `ref_guard` structure goes out of scope.
 *
 * @guard: The guard lock
 */
void drop_cs_unref(struct ref_guard *guard) {
    guard->status = -EBADF;
    guard->unref(guard->cs);
}

/**
 * Destroy the clip store, releasing all of its resources.
 *
 * @cs: The clip store to operate on
 */
int cs_destroy(struct clip_store *cs) {
    cs->ready = false;
    // Don't use the value from the header: if it's out of date, we haven't
    // done mremap() with the new size yet
    if (munmap(cs->header, cs_file_size(cs->local_nr_snips_alloc))) {
        return negative_errno();
    }
    return 0;
}

/**
 * _drop_() function for when a `clip_store` goes out of scope.
 *
 * @guard: The guard lock
 */
void drop_cs_destroy(struct clip_store *cs) { expect(cs_destroy(cs) == 0); }

/**
 * Initialise a `struct clip_store` with snip_fd open to a file for snip
 * storage and content_fd open to a directory for content entry storage.
 *
 * The snip file is extended and the header snip is written if the file size is
 * zero. The file is mapped into memory until cs_destroy() is called.
 *
 * @cs: The clip store to initialise
 * @snip_fd: Open file descriptor for the snip file
 * @content_dir_fd: Open file descriptor for the content directory
 */
int cs_init(struct clip_store *cs, int snip_fd, int content_dir_fd) {
    cs->ready = false;
    cs->snip_fd = snip_fd;
    cs->content_dir_fd = content_dir_fd;
    cs->refcount = 0;
    _drop_(cs_unref) struct ref_guard guard = cs_ref_no_update(cs);

    struct stat st;
    if (fstat(snip_fd, &st) < 0) {
        return negative_errno();
    }
    if (st.st_size % CS_SNIP_SIZE != 0) {
        return -EINVAL;
    }

    size_t file_size = (size_t)st.st_size;
    if (file_size == 0) {
        file_size = CS_SNIP_SIZE;
        if (ftruncate(snip_fd, (off_t)file_size) < 0) {
            return negative_errno();
        }
    }

    cs->header =
        mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, snip_fd, 0);
    if (cs->header == MAP_FAILED) {
        return negative_errno();
    }

    int ret = cs_header_validate(cs, file_size);
    if (ret < 0) {
        munmap(cs->header, file_size);
        return ret;
    }

    cs->snips = (struct cs_snip *)(cs->header + 1);
    cs->local_nr_snips = cs->header->nr_snips;
    cs->local_nr_snips_alloc = cs->header->nr_snips_alloc;
    cs->ready = true;

    (void)guard; // Old clang will complain guard is unused, despite cleanup

    return 0;
}

/**
 * Round up a number to the nearest multiple of a specified step.
 *
 * @n: The number to round up
 * @step: The step size to round up to
 */
static size_t round_up(size_t n, size_t step) {
    return ((n + step - 1) / step) * step;
}

/**
 * Adjust the size of the mmapped file used by the clip store to the specified
 * new size. It first attempts to resize the file using ftruncate(). If this
 * operation is successful, it then remaps the memory mapping to reflect the
 * new size of the file.
 *
 * WARNING: cs->snips and cs->header may move after cs_file_resize(), so
 * copied pointers from before invocation must not be used after calling this.
 *
 * @cs: The clip store to operate on
 * @new_nr_snips: The new number of snips in the snip file
 */
static int _must_use_ _nonnull_ cs_file_resize(struct clip_store *cs,
                                               size_t new_nr_snips) {
    bool grow = new_nr_snips >= cs->header->nr_snips;

    if (grow && new_nr_snips <= cs->header->nr_snips_alloc) {
        cs->header->nr_snips = new_nr_snips;
        return 0;
    }

    /* If this is a shrink, do it exactly: someone may be deleting sensitive
     * snips and so it's best to remove them immediately. Otherwise, batch
     * allocate. */
    size_t new_nr_snips_alloc =
        grow ? round_up(new_nr_snips, CS_SNIP_ALLOC_BATCH) : new_nr_snips;

    size_t new_size = cs_file_size(new_nr_snips_alloc);
    if (ftruncate(cs->snip_fd, (off_t)new_size) < 0) {
        return negative_errno();
    }

    if (grow) {
        struct cs_header *new_snips =
            mremap(cs->header, cs_file_size(cs->header->nr_snips_alloc),
                   new_size, MREMAP_MAYMOVE);
        if (new_snips == MAP_FAILED) {
            return negative_errno();
        }
        cs->header = new_snips;
        cs->snips = (struct cs_snip *)cs->header + 1;
    }

    cs->header->nr_snips = cs->local_nr_snips = new_nr_snips;
    cs->header->nr_snips_alloc = cs->local_nr_snips_alloc = new_nr_snips_alloc;

    return 0;
}

/**
 * Update a clip store snip to contain the specified hash and line content.
 *
 * @snip: Pointer to the snip to modify
 * @hash: The new hash value for the snip
 * @line: The new line content for the snip
 * @nr_lines: The number of lines in the line content
 */
static void _nonnull_ cs_snip_update(struct cs_snip *snip, uint64_t hash,
                                     const char *line, uint64_t nr_lines) {
    snip->hash = hash;
    snip->doomed = false;
    snip->nr_lines = nr_lines;
    strncpy(snip->line, line, CS_SNIP_LINE_SIZE - 1);
    snip->line[CS_SNIP_LINE_SIZE - 1] = '\0';
}

/**
 * Computes a 64-bit FNV-1a hash for a given buffer.
 *
 * @buf: The input buffer to hash.
 */
static uint64_t fnv1a_64_hash(const char *buf) {
    const uint64_t fnv_offset_basis = 0xcbf29ce484222325ULL;
    const uint64_t fnv_prime = 0x100000001b3ULL;
    uint64_t hash = fnv_offset_basis;
    const uint8_t *src = (const uint8_t *)buf;
    while (*src) {
        hash ^= *src++;
        hash *= fnv_prime;
    }
    return hash;
}

/**
 * Extracts the first non-empty line from a given text buffer and copies it to
 * the output buffer. Returns the total number of lines. A final line with no
 * newline is considered a line for accounting purposes.
 *
 * @text: The input text buffer
 * @out: The output buffer. Must be at least CS_SNIP_LINE_SIZE bytes
 */
size_t first_line(const char *text, char *out) {
    bool found = false;
    size_t nr_lines = 0;
    const char *cur = text;

    out[0] = '\0';

    for (; *cur; cur++) {
        nr_lines += (*cur == '\n');
        if (!found && *cur != '\n') {
            found = true;
            snprintf(out, CS_SNIP_LINE_SIZE, "%.*s", (int)strcspn(cur, "\n"),
                     cur);
        }
    }

    return nr_lines + (found && *--cur != '\n');
}

/**
 * Add a new snip consisting of a hash value and a line of text to the end of
 * the clip store. The snip file size is grown as necessary to accommodate the
 * new snip.
 *
 * @cs: The clip store to operate on
 * @hash: The hash value of the snip to add
 * @line: The line content of the snip to add
 * @nr_lines: The number of lines in the line content
 */
static int _must_use_ _nonnull_ cs_snip_add(struct clip_store *cs,
                                            uint64_t hash, const char *line,
                                            uint64_t nr_lines) {
    _drop_(cs_unref) struct ref_guard guard = cs_ref(cs);
    if (guard.status < 0) {
        return guard.status;
    }
    int ret = cs_file_resize(cs, cs->header->nr_snips + 1);
    if (ret < 0) {
        return ret;
    }
    cs_snip_update(cs->snips + cs->header->nr_snips - 1, hash, line, nr_lines);
    return 0;
}

/**
 * Add content to the content directory using the hash as the filename.
 *
 * @cs: The clip store to operate on
 * @hash: The hash of the content to add
 * @content: The content to add to the file
 * @dupe_policy: If set to CS_DUPE_KEEP_LAST, will return with -EEXIST when
 * trying to insert duplicate entry.
 */
static int _must_use_ _nonnull_
cs_content_add(struct clip_store *cs, uint64_t hash, const char *content,
               enum cs_dupe_policy dupe_policy) {
    bool dupe = false;
    size_t content_len = strlen(content);

    char dir_path[CS_HASH_STR_MAX];
    snprintf(dir_path, sizeof(dir_path), PRI_HASH, hash);

    int ret = mkdirat(cs->content_dir_fd, dir_path, 0700);
    if (ret < 0) {
        if (errno != EEXIST || dupe_policy == CS_DUPE_KEEP_LAST) {
            return negative_errno();
        }
        dupe = true;
    }

    char base_file_path[PATH_MAX];
    snprintf(base_file_path, sizeof(base_file_path), "%s/1", dir_path);

    if (dupe) {
        // This clip already exists, just create a link for refcounting
        struct stat st;
        if (fstatat(cs->content_dir_fd, base_file_path, &st, 0) < 0) {
            return negative_errno();
        }

        if ((size_t)st.st_size != content_len) {
            // Extremely unlikely with FNV-1a 64-bit outside of artificial
            // scenarios, but never hurts to be careful...
            return -EFAULT;
        }

        size_t link_num = (size_t)st.st_nlink + 1;
        char linkpath[PATH_MAX];
        snprintf(linkpath, sizeof(linkpath), "%s/%zu", dir_path, link_num);
        if (linkat(cs->content_dir_fd, base_file_path, cs->content_dir_fd,
                   linkpath, 0) < 0) {
            return negative_errno();
        }

        return 0;
    }

    // This is a new clip
    _drop_(close) int fd = openat(cs->content_dir_fd, base_file_path,
                                  O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return negative_errno();
    }

    const char *cur = content;
    size_t remaining = content_len;

    while (remaining > 0) {
        ssize_t written = write(fd, cur, remaining);
        if (written < 0) {
            return negative_errno();
        }
        remaining -= (size_t)written;
        cur += written;
    }

    return 0;
}

/**
 * Clean up the backing mapped data and file descriptor for a `struct
 * cs_content` object.
 *
 * @content: The content to unmap
 */
int cs_content_unmap(struct cs_content *content) {
    if (content && content->data) {
        close(content->fd);
        if (munmap(content->data, (size_t)content->size)) {
            return negative_errno();
        }
    }
    return 0;
}

/**
 * _drop_ function for cs_content_unmap()
 *
 * @content: The content to unmap
 */
void drop_cs_content_unmap(struct cs_content *content) {
    int ret = cs_content_unmap(content);
    expect(ret == 0);
}

/**
 * Retrieve the content associated with a given hash from the content directory
 * and map it into memory.
 *
 * @cs: The clip store to operate on
 * @hash: The hash of the content to retrieve
 * @content: A pointer to a `struct cs_content` to populate. The caller must
 *           call cs_content_unmap() when done to free it
 */
int cs_content_get(struct clip_store *cs, uint64_t hash,
                   struct cs_content *content) {
    memset(content, '\0', sizeof(struct cs_content));

    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), PRI_HASH "/1", hash);

    _drop_(close) int fd = openat(cs->content_dir_fd, filename, O_RDONLY);
    if (fd < 0) {
        return negative_errno();
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        return negative_errno();
    }

    char *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        return negative_errno();
    }

    content->data = data;
    content->fd = fd;
    content->size = st.st_size;

    return 0;
}

/**
 * Move the entry with the specified hash to the newest slot.
 *
 * @cs: The clip store to operate on
 * @hash: The hash of the entry to move
 */
int cs_make_newest(struct clip_store *cs, uint64_t hash) {
    _drop_(cs_unref) struct ref_guard guard = cs_ref(cs);
    if (guard.status < 0) {
        return guard.status;
    }

    for (int i = 0; i < (int)cs->local_nr_snips; ++i) {
        if (cs->snips[i].hash == hash) {
            struct cs_snip tmp = cs->snips[i];
            memmove(cs->snips + i, cs->snips + i + 1,
                    (cs->local_nr_snips - (i + 1)) * sizeof(*cs->snips));
            cs->snips[cs->local_nr_snips - 1] = tmp;
            return 0;
        }
    }
    die("unreachable");
}

/**
 * Add a new content entry to the clip store and content directory.
 *
 * @cs: The clip store to operate on
 * @content: The content to add
 * @out_hash: Output for the generated hash, or NULL
 * @dupe_policy: Policy to use for duplicate entries
 */
int cs_add(struct clip_store *cs, const char *content, uint64_t *out_hash,
           enum cs_dupe_policy dupe_policy) {
    uint64_t hash = fnv1a_64_hash(content);
    char line[CS_SNIP_LINE_SIZE];
    size_t nr_lines = first_line(content, line);

    if (out_hash) {
        *out_hash = hash;
    }

    int ret = cs_content_add(cs, hash, content, dupe_policy);
    if (ret == -EEXIST && dupe_policy == CS_DUPE_KEEP_LAST) {
        return cs_make_newest(cs, hash);
    }
    return ret ? ret : cs_snip_add(cs, hash, line, nr_lines);
}

/**
 * Iterate over the snips in the clip store. The function should be initially
 * called with *snip set to NULL, which will set *snip to point to the newest
 * snip (excluding the header). On subsequent calls, *snip is updated to point
 * to the next snip in the snip file. The iteration stops when there are no
 * more snips to process, indicated by the function returning false.
 *
 * @guard: The guard lock
 * @snip: Pointer to a pointer to the current snip being iterated over
 * @direction: Whether to iterate from the oldest to newest or vice versa
 */
bool cs_snip_iter(struct ref_guard *guard, enum cs_iter_direction direction,
                  struct cs_snip **snip) {
    if (guard->status < 0 || guard->cs->header->nr_snips == 0) {
        return false;
    }

    struct cs_snip *oldest = guard->cs->snips;
    struct cs_snip *newest = oldest + (guard->cs->header->nr_snips - 1);
    const struct cs_snip *stop =
        direction == CS_ITER_NEWEST_FIRST ? oldest : newest;

    if (!*snip) {
        *snip = direction == CS_ITER_NEWEST_FIRST ? newest : oldest;
        return true;
    } else if (*snip != stop) {
        *snip = *snip + (direction == CS_ITER_NEWEST_FIRST ? -1 : 1);
        return true;
    }
    return false;
}

/**
 * Remove content from the content directory using the hash as the filename.
 *
 * @cs: The clip store to operate on
 * @hash: The hash of the content to remove
 */
static int _must_use_ _nonnull_ cs_content_remove(struct clip_store *cs,
                                                  uint64_t hash) {

    char hash_dir_name[CS_HASH_STR_MAX];
    snprintf(hash_dir_name, sizeof(hash_dir_name), PRI_HASH, hash);

    _drop_(close) int hash_dir_fd =
        openat(cs->content_dir_fd, hash_dir_name, O_RDONLY);
    if (hash_dir_fd < 0) {
        return negative_errno();
    }

    struct stat st;
    if (fstatat(hash_dir_fd, "1", &st, 0) < 0) {
        return negative_errno();
    }

    char nlink_path[PATH_MAX];
    snprintf(nlink_path, sizeof(nlink_path), "%u", (unsigned)st.st_nlink);

    if (unlinkat(hash_dir_fd, nlink_path, 0) < 0) {
        return negative_errno();
    }

    if (st.st_nlink == 1 &&
        unlinkat(cs->content_dir_fd, hash_dir_name, AT_REMOVEDIR) < 0) {
        return negative_errno();
    }

    return 0;
}

/**
 * Compacts the clip store by removing doomed snips, finalising their removal
 * after being marked in cs_remove().
 *
 * @guard: The guard lock
 */
static size_t _nonnull_ cs_snip_remove_doomed(struct ref_guard *guard) {
    size_t nr_doomed = 0;
    struct cs_snip *snip = NULL;

    while (cs_snip_iter(guard, CS_ITER_OLDEST_FIRST, &snip)) {
        if (snip->doomed) {
            nr_doomed++;
        } else if (nr_doomed > 0) {
            *(snip - nr_doomed) = *snip;
        }
    }

    return nr_doomed;
}

/**
 * Iterate over the specified number of snips in the clip store from newest
 * to oldest and remove those for which the predicate function returns
 * CS_ACTION_REMOVE (see `enum cs_remove_action` above).
 *
 * @cs: The clip store to operate on
 * @should_remove: Function pointer to the predicate used to decide removal
 * @private: Pointer to user-defined data passed to the predicate function
 * @direction: Whether to iterate from the oldest to newest or vice versa
 */
int cs_remove(struct clip_store *cs, enum cs_iter_direction direction,
              enum cs_remove_action (*should_remove)(uint64_t, const char *,
                                                     void *),
              void *private) {
    _drop_(cs_unref) struct ref_guard guard = cs_ref(cs);
    if (guard.status < 0) {
        return guard.status;
    }

    bool found = false;
    struct cs_snip *snip = NULL;

    while (cs_snip_iter(&guard, direction, &snip)) {
        enum cs_remove_action action =
            should_remove(snip->hash, snip->line, private);

        if (action & CS_ACTION_REMOVE) {
            found = true;
            int ret = cs_content_remove(cs, snip->hash);
            if (ret < 0) {
                return ret;
            }
            snip->doomed = true;
        }
        if (action & CS_ACTION_STOP) {
            break;
        }
    }

    if (!found) {
        return 0;
    }

    size_t nr_doomed = cs_snip_remove_doomed(&guard);
    int ret = cs_file_resize(cs, cs->header->nr_snips - nr_doomed);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/**
 * Callback function for deciding which snips to remove during trim operation.
 *
 * @hash: Unused
 * @line: Unused
 * @private: The count of remaining entries to trim
 */
static enum cs_remove_action _must_use_ _nonnull_
trim_callback(uint64_t hash, const char *line, void *private) {
    (void)hash;
    (void)line;

    size_t *count = private;
    if (*count == 0) {
        return CS_ACTION_REMOVE;
    }
    (*count)--;
    return CS_ACTION_KEEP;
}

/**
 * Trim the clip store to only retain the specified number of snips.
 *
 * @cs: The clip store to operate on
 * @direction: Whether to remove the N newest or N oldest
 * @nr_keep: The number of newest snips to retain
 */
int cs_trim(struct clip_store *cs, enum cs_iter_direction direction,
            size_t nr_keep) {
    if (nr_keep >= cs->header->nr_snips) {
        return 0; // No action needed if we're keeping everything or more
    }

    int ret = cs_remove(cs, direction, trim_callback, &nr_keep);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/**
 * Replace the content and snip for an entry in the clip store, identified by
 * its age.
 *
 * @cs: The clip store to operate on
 * @age: The age of the snip to replace, with 0 being the newest
 * @direction: Whether to iterate from the oldest to newest or vice versa
 * @content: The content to replace this entry with
 * @out_hash: Output for the generated hash, or NULL
 */
int cs_replace(struct clip_store *cs, enum cs_iter_direction direction,
               size_t age, const char *content, uint64_t *out_hash) {
    _drop_(cs_unref) struct ref_guard guard = cs_ref(cs);
    if (guard.status < 0) {
        return guard.status;
    }

    if (age >= cs->header->nr_snips) {
        return -ERANGE;
    }

    size_t idx = direction == CS_ITER_NEWEST_FIRST
                     ? cs->header->nr_snips - age - 1
                     : age;
    struct cs_snip *snip = cs->snips + idx;

    int ret = cs_content_remove(cs, snip->hash);
    if (ret) {
        return ret;
    }
    char line[CS_SNIP_LINE_SIZE];
    size_t nr_lines = first_line(content, line);
    uint64_t hash = fnv1a_64_hash(content);
    cs_snip_update(snip, hash, line, nr_lines);
    ret = cs_content_add(cs, hash, content, CS_DUPE_KEEP_ALL);
    if (ret) {
        return ret;
    }
    if (out_hash) {
        *out_hash = hash;
    }
    return 0;
}

/**
 * Get the current number of entries in the clip store.
 *
 * @cs: The clip store to operate on
 * @out_len: Output for the length of the clip store
 */
int cs_len(struct clip_store *cs, size_t *out_len) {
    _drop_(cs_unref) struct ref_guard guard = cs_ref(cs);
    if (guard.status < 0) {
        return guard.status;
    }
    *out_len = cs->header->nr_snips;
    return 0;
}
