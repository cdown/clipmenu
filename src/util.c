#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "config.h"
#include "store.h"
#include "util.h"

/**
 * Write data to a file descriptor, ensuring all bytes are written.
 */
void write_safe(int fd, const char *buf, size_t count) {
    while (count > 0) {
        ssize_t chunk_size = write(fd, buf, count);
        expect(chunk_size >= 0);
        buf += chunk_size;
        expect(chunk_size <= (ssize_t)count);
        count -= (size_t)chunk_size;
    }
}

/**
 * Read data from a file descriptor into a buffer safely, ensuring correct
 * handling of partial reads.
 */
size_t read_safe(int fd, char *buf, size_t count) {
    size_t count_start = count;
    while (count > 0) {
        ssize_t chunk_size = read(fd, buf, count);
        expect(chunk_size >= 0);
        if (chunk_size == 0) { // EOF
            break;
        }
        buf += chunk_size;
        count -= (size_t)chunk_size;
    }
    expect(count_start >= count);
    return count_start - count;
}

/**
 * Performs safe, bounded string formatting into a buffer. On error or
 * truncation, expect() aborts.
 */
size_t snprintf_safe(char *buf, size_t len, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(buf, len, fmt, args);
    va_end(args);
    expect(needed >= 0 && (size_t)needed < len);
    return (size_t)needed;
}

/**
 * Runs clipserve to handle selection requests for a hash in the clip store.
 */
#define CLIPSERVE_MAX_ARGS (CM_SEL_MAX * 2 + 3)

void run_clipserve(uint64_t hash, const struct selection *selections) {
    char hash_str[CS_HASH_STR_MAX];
    snprintf(hash_str, sizeof(hash_str), PRI_HASH, hash);

    const char *cmd[CLIPSERVE_MAX_ARGS];
    size_t cmd_idx = 0;
    cmd[cmd_idx++] = "clipserve";
    if (selections) {
        for (size_t i = 0; i < CM_SEL_MAX; i++) {
            if (!selections[i].active) {
                continue;
            }
            expect(cmd_idx + 2 < CLIPSERVE_MAX_ARGS);
            cmd[cmd_idx++] = "--selection";
            cmd[cmd_idx++] = selections[i].name;
        }
    }
    cmd[cmd_idx++] = hash_str;
    cmd[cmd_idx++] = NULL;
    pid_t pid = fork();
    expect(pid >= 0);

    if (pid > 0) {
        return;
    }

    execvp(cmd[0], (char *const *)cmd);
    die("Failed to exec %s: %s\n", cmd[0], strerror(errno));
}

/**
 * Convert a positive errno value to a negative error code, ensuring a
 * non-zero value is returned.
 *
 * This is needed because clang-tidy and gcc may complain when doing plain
 * "return -errno" because the compiler does not know that errno cannot be 0
 * (and thus that later checks with func() == 0 cannot pass in error
 * situations).
 */
int negative_errno(void) { return errno > 0 ? -errno : -EINVAL; }

/**
 * Convert a string to an unsigned 64-bit integer in given base, validating the
 * format and range of the input.
 */
static int str_to_uint64_base(const char *input, uint64_t *output, int base) {
    char *endptr;
    errno = 0;

    uint64_t val = strtoull(input, &endptr, base);
    if (errno > 0) {
        return negative_errno();
    }
    if (!endptr || endptr == input || *endptr != 0) {
        return -EINVAL;
    }
    if (val != 0 && input[0] == '-') {
        return -ERANGE;
    }

    *output = val;
    return 0;
}

/**
 * Convert a string to an unsigned 64-bit integer, validating the format and
 * range of the input.
 */
int str_to_uint64(const char *input, uint64_t *output) {
    return str_to_uint64_base(input, output, 10);
}

/**
 * Convert a hex string to an unsigned 64-bit integer, validating the format
 * and range of the input.
 */
int str_to_hex64(const char *input, uint64_t *output) {
    return str_to_uint64_base(input, output, 16);
}

/**
 * Check whether debug mode is enabled and cache the result. Used for dbg().
 */
bool debug_mode_enabled(void) {
    static int debug_enabled = -1;
    if (debug_enabled == -1) {
        const char *dbg_env = getenv("CM_DEBUG");
        debug_enabled = dbg_env && streq(dbg_env, "1");
    }
    return debug_enabled;
}

/**
 * Compute a 64-bit FNV-1a hash for a NULL-terminated string.
 */
uint64_t fnv1a_64_hash(const char *buf) {
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
