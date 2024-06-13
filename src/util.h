#ifndef CM_UTIL_H
#define CM_UTIL_H

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define _drop_(x) __attribute__((__cleanup__(drop_##x)))
#define _must_use_ __attribute__((warn_unused_result))
#define _nonnull_ __attribute__((nonnull))
#define _nonnull_n_(...) __attribute__((nonnull(__VA_ARGS__)))
#define _noreturn_ __attribute__((noreturn))
#define _packed_ __attribute__((packed))
#define _printf_(a, b) __attribute__((__format__(printf, a, b)))
#define _unused_ __attribute__((__unused__))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#ifndef __cplusplus
    #define static_assert _Static_assert
#endif

#define UINT64_MAX_STRLEN 20 // (1 << 64) - 1 is 20 digits wide

#define streq(a, b) (strcmp((a), (b)) == 0)
#define strceq(a, b) (strcasecmp((a), (b)) == 0)
#define strnull(s) (s) ? (s) : "[null]"

#define arrlen(x)                                                              \
    (__builtin_choose_expr(                                                    \
        !__builtin_types_compatible_p(typeof(x), typeof(&*(x))),               \
        sizeof(x) / sizeof((x)[0]), (void)0 /* decayed, compile error */))

#define _die(dump, fmt, ...)                                                   \
    do {                                                                       \
        fprintf(stderr, "FATAL: " fmt, ##__VA_ARGS__);                         \
        if (dump) {                                                            \
            abort();                                                           \
        }                                                                      \
        exit(1);                                                               \
    } while (0)
#define die(fmt, ...) _die(0, fmt, ##__VA_ARGS__)
#define die_on(cond, fmt, ...)                                                 \
    do {                                                                       \
        if (unlikely(cond)) {                                                  \
            die(fmt, ##__VA_ARGS__);                                           \
        }                                                                      \
    } while (0)
#define expect(x)                                                              \
    do {                                                                       \
        if (!likely(x)) {                                                      \
            _die(1, "!(%s) at %s:%s:%d\n", #x, __FILE__, __func__, __LINE__);  \
        }                                                                      \
    } while (0)

#define dbg(fmt, ...)                                                          \
    do {                                                                       \
        if (debug_mode_enabled()) {                                            \
            fprintf(stderr, "%s:%ld:%s:%s:%d: " fmt, prog_name,                \
                    (long)getpid(), __FILE__, __func__, __LINE__,              \
                    ##__VA_ARGS__);                                            \
        }                                                                      \
    } while (0)

void _nonnull_ write_safe(int fd, const char *buf, size_t count);
size_t _nonnull_ read_safe(int fd, char *buf, size_t count);
size_t _printf_(3, 4)
    snprintf_safe(char *buf, size_t len, const char *fmt, ...);

void run_clipserve(uint64_t hash);

/**
 * __attribute__((cleanup)) functions
 */
#define DEFINE_DROP_FUNC_PTR(type, func)                                       \
    static inline void drop_##func(type *p) { func(p); }
#define DEFINE_DROP_FUNC(type, func)                                           \
    static inline void drop_##func(type *p) {                                  \
        if (*p)                                                                \
            func(*p);                                                          \
    }
#define DEFINE_DROP_FUNC_VOID(func)                                            \
    static inline void drop_##func(void *p) {                                  \
        void **pp = p;                                                         \
        if (*pp)                                                               \
            func(*pp);                                                         \
    }

static inline void drop_close(int *fd) {
    if (*fd >= 0) {
        close(*fd);
    }
}

DEFINE_DROP_FUNC_VOID(free)
DEFINE_DROP_FUNC(FILE *, fclose)
DEFINE_DROP_FUNC(DIR *, closedir)

int _must_use_ negative_errno(void);
int _nonnull_ str_to_uint64(const char *input, uint64_t *output);
int _nonnull_ str_to_hex64(const char *input, uint64_t *output);
bool debug_mode_enabled(void);

#endif
