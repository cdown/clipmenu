#undef NDEBUG

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/store.h"
#include "../src/util.h"

#define COL_NORMAL "\x1B[0m"
#define COL_GREEN "\x1B[32m"
#define COL_RED "\x1B[31m"

#define t_assert(test)                                                         \
    do {                                                                       \
        if (!(test)) {                                                         \
            printf("  %s[FAIL:%d] %s%s\n", COL_RED, __LINE__, #test,           \
                   COL_NORMAL);                                                \
            return false;                                                      \
        }                                                                      \
        printf("  %s[PASS:%d] %s%s\n", COL_GREEN, __LINE__, #test,             \
               COL_NORMAL);                                                    \
    } while (0)

#define t_run(test)                                                            \
    do {                                                                       \
        printf("%s:\n", #test);                                                \
        bool ret = test();                                                     \
        printf("\n");                                                          \
        if (!ret) {                                                            \
            return ret;                                                        \
        }                                                                      \
    } while (0)

#define TEST_SNIP_FILE "/clip_store_snip_test"
#define TEST_CONTENT_DIR "/dev/shm/clip_store_content_dir_test"

static int create_test_snip_fd(void) {
    shm_unlink(TEST_SNIP_FILE);
    int snip_fd = shm_open(TEST_SNIP_FILE, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(snip_fd >= 0);
    return snip_fd;
}

static void drop_remove_test_snip_fd(int *snip_fd) {
    close(*snip_fd);
    int ret = shm_unlink(TEST_SNIP_FILE);
    assert(ret == 0);
}

static void _drop_remove_test_content_dir_fd(int *dir_fd_ptr) {
    int dir_fd = *dir_fd_ptr;
    DIR *dir = fdopendir(dir_fd);
    if (dir == NULL) {
        close(dir_fd);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (streq(entry->d_name, ".") || streq(entry->d_name, "..")) {
            continue;
        }

        if (entry->d_type == DT_REG) {
            int ret = unlinkat(dir_fd, entry->d_name, 0);
            assert(ret == 0);
        } else if (entry->d_type == DT_DIR) {
            int subdir_fd =
                openat(dir_fd, entry->d_name, O_RDONLY | O_DIRECTORY);
            assert(subdir_fd >= 0);
            _drop_remove_test_content_dir_fd(&subdir_fd);
            int ret = unlinkat(dir_fd, entry->d_name, AT_REMOVEDIR);
            assert(ret == 0);
        }
    }

    closedir(dir);
}

static void drop_remove_test_content_dir_fd(int *dir_fd_ptr) {
    _drop_remove_test_content_dir_fd(dir_fd_ptr);
    int ret = rmdir(TEST_CONTENT_DIR);
    assert(ret == 0);
}

static void remove_test_content_dir(const char *path) {
    int dir_fd = open(path, O_RDONLY);
    if (dir_fd < 0) {
        assert(errno == ENOENT);
        return;
    }
    drop_remove_test_content_dir_fd(&dir_fd);
}

static int create_test_content_dir_fd(void) {
    remove_test_content_dir(TEST_CONTENT_DIR);
    int ret = mkdir(TEST_CONTENT_DIR, 0700);
    assert(ret == 0);
    int dir_fd = open(TEST_CONTENT_DIR, O_RDONLY);
    assert(dir_fd >= 0);
    return dir_fd;
}

/* Test callables */
static enum cs_remove_action remove_if_five(uint64_t hash, const char *line,
                                            void *private) {
    (void)hash;
    size_t *count = private;
    enum cs_remove_action ret = 0;

    if (*count == 0) {
        ret |= CS_ACTION_STOP;
    }
    (*count)--;

    if (line[0] == '5') {
        ret |= CS_ACTION_REMOVE;
    }

    return ret;
}

static struct clip_store setup_test(void) {
    struct clip_store cs;
    int snip_fd = create_test_snip_fd();
    int content_dir_fd = create_test_content_dir_fd();
    int ret = cs_init(&cs, snip_fd, content_dir_fd);
    assert(ret == 0);
    return cs;
}

static void drop_teardown_test(struct clip_store *cs) {
    int snip_fd = cs->snip_fd;
    int content_dir_fd = cs->content_dir_fd;
    int ret = cs_destroy(cs);
    assert(ret == 0);
    close(snip_fd);
    shm_unlink(TEST_SNIP_FILE);
    close(content_dir_fd);
    remove_test_content_dir(TEST_CONTENT_DIR);
}

static void add_ten_snips(struct clip_store *cs) {
    for (char i = 0; i < 10; i++) {
        char num[8];
        snprintf(num, sizeof(num), "%d", i);
        int ret = cs_add(cs, num, NULL, CS_DUPE_KEEP_ALL);
        assert(ret == 0);
    }
}

/* Tests */

static bool test__cs_init(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    t_assert(cs.snip_fd >= 0);
    t_assert(cs.content_dir_fd >= 0);

    /* Check header fields were set up and are correct */
    struct stat st;
    t_assert(fstat(cs.snip_fd, &st) == 0);
    t_assert(st.st_size == CS_SNIP_SIZE);
    t_assert(cs.header->nr_snips_alloc == 0);

    return true;
}

static bool test__cs_init__bad_size(void) {
    _drop_(remove_test_snip_fd) int snip_fd = create_test_snip_fd();
    _drop_(remove_test_content_dir_fd) int content_dir_fd =
        create_test_content_dir_fd();
    t_assert(ftruncate(snip_fd, CS_SNIP_SIZE - 1) == 0);
    struct clip_store cs;
    t_assert(cs_init(&cs, snip_fd, content_dir_fd) == -EINVAL);

    return true;
}

static bool test__cs_init__bad_size_aligned(void) {
    _drop_(remove_test_snip_fd) int snip_fd = create_test_snip_fd();
    _drop_(remove_test_content_dir_fd) int content_dir_fd =
        create_test_content_dir_fd();
    t_assert(ftruncate(snip_fd, CS_SNIP_SIZE * 2) == 0);
    struct clip_store cs;
    t_assert(cs_init(&cs, snip_fd, content_dir_fd) == -EINVAL);

    return true;
}

static bool test__cs_add(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    for (char i = 0; i < 10; i++) {
        char num[8];
        snprintf(num, sizeof(num), "%d", i);

        uint64_t hash;
        int ret = cs_add(&cs, num, &hash, CS_DUPE_KEEP_ALL);
        t_assert(ret == 0);

        _drop_(cs_content_unmap) struct cs_content content;
        ret = cs_content_get(&cs, hash, &content);
        t_assert(ret == 0);
        t_assert(strncmp(content.data, num, content.size) == 0);
    }

    t_assert(cs.header->nr_snips == 10);

    return true;
}

static bool test__cs_snip_iter(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();
    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);

    add_ten_snips(&cs);

    struct cs_snip *snip = NULL;
    int last_num = 9;
    while (cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip)) {
        _drop_(cs_content_unmap) struct cs_content content;
        int ret = cs_content_get(&cs, snip->hash, &content);
        t_assert(ret == 0);

        /* add_ten_snips adds from 0-9, we should get the last one first */
        int num = content.data[0] - '0';
        t_assert(num == last_num--);
    }

    snip = NULL;
    last_num = 0;
    while (cs_snip_iter(&guard, CS_ITER_OLDEST_FIRST, &snip)) {
        _drop_(cs_content_unmap) struct cs_content content;
        int ret = cs_content_get(&cs, snip->hash, &content);
        t_assert(ret == 0);

        int num = content.data[0] - '0';
        t_assert(num == last_num++);
    }

    return true;
}

static bool test__cs_remove(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();
    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);

    add_ten_snips(&cs);

    size_t nr_iter = 1;
    t_assert(cs_remove(&cs, CS_ITER_NEWEST_FIRST, remove_if_five, &nr_iter) ==
             0);
    t_assert(cs.header->nr_snips == 10);
    nr_iter = SIZE_MAX;
    t_assert(cs_remove(&cs, CS_ITER_NEWEST_FIRST, remove_if_five, &nr_iter) ==
             0);
    t_assert(cs.header->nr_snips == 9);

    return true;
}

static bool test__cs_trim(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();
    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);

    add_ten_snips(&cs);

    struct cs_snip *snip = NULL;
    while (cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip))
        ;
    uint64_t oldest_hash = snip->hash;

    t_assert(cs_trim(&cs, CS_ITER_NEWEST_FIRST, 3) == 0);
    t_assert(cs.header->nr_snips == 3);

    /* Check we kept the most recently added ones */
    int last_num = 9;
    snip = NULL;
    while (cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip)) {
        _drop_(cs_content_unmap) struct cs_content content;
        int ret = cs_content_get(&cs, snip->hash, &content);
        t_assert(ret == 0);

        int num = content.data[0] - '0';
        t_assert(num == last_num--);
    }

    /* Check the oldest snip hash is gone */
    _drop_(cs_content_unmap) struct cs_content content;
    t_assert(cs_content_get(&cs, oldest_hash, &content) == -ENOENT);

    return true;
}

static bool test__cs_replace(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();
    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);

    add_ten_snips(&cs);

    const char *new = "new";

    int ret = cs_replace(&cs, CS_ITER_NEWEST_FIRST, 1, new, NULL);
    t_assert(ret == 0);

    struct cs_snip *snip = NULL;
    bool i_ret = cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip);
    t_assert(i_ret == true);
    t_assert(streq(snip->line, "9"));
    i_ret = cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip);
    t_assert(i_ret == true);
    t_assert(streq(snip->line, new));

    _drop_(cs_content_unmap) struct cs_content first_content;
    ret = cs_content_get(&cs, snip->hash, &first_content);
    t_assert(ret == 0);
    t_assert(strncmp(first_content.data, new, first_content.size) == 0);

    /* No other clips should be affected */
    int last_num = 7;
    while (cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip)) {
        _drop_(cs_content_unmap) struct cs_content content;
        ret = cs_content_get(&cs, snip->hash, &content);
        t_assert(ret == 0);

        int num = content.data[0] - '0';
        t_assert(num == last_num--);
    }

    return true;
}

static bool test__reuse_cs(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    add_ten_snips(&cs);
    t_assert(cs_destroy(&cs) == 0);

    t_assert(cs_init(&cs, cs.snip_fd, cs.content_dir_fd) == 0);
    t_assert(cs.header->nr_snips == 10);

    return true;
}

static bool test__cs_add__exceeds_snip_line_size(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    /* Construct a string that exceeds CS_SNIP_LINE_SIZE */
    char long_content[CS_SNIP_LINE_SIZE + 100];
    memset(long_content, 'A', sizeof(long_content));
    long_content[sizeof(long_content) - 1] = '\0';

    int ret = cs_add(&cs, long_content, NULL, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);

    struct cs_snip *snip = NULL;
    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);
    bool found = cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip);
    t_assert(found);
    t_assert(strlen(snip->line) == CS_SNIP_LINE_SIZE - 1);

    return true;
}

static bool test__cs_trim__when_empty(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    int ret = cs_trim(&cs, CS_ITER_NEWEST_FIRST, 0);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 0);

    return true;
}

static bool test__cs_remove___empty(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    /* Attempt to remove any entry, if present */
    size_t dummy = 0;
    int ret = cs_remove(&cs, CS_ITER_NEWEST_FIRST, remove_if_five, &dummy);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 0);

    return true;
}

static bool test__cs_add__around_alloc_batch_threshold(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    for (size_t i = 0; i < CS_SNIP_ALLOC_BATCH - 1; i++) {
        int ret = cs_add(&cs, "test content", NULL, CS_DUPE_KEEP_ALL);
        assert(ret == 0);
    }
    t_assert(cs.header->nr_snips == CS_SNIP_ALLOC_BATCH - 1);

    /* Add one more entry to exceed the batch threshold */
    t_assert(cs_add(&cs, "test content", NULL, CS_DUPE_KEEP_ALL) == 0);
    t_assert(cs.header->nr_snips == CS_SNIP_ALLOC_BATCH);
    t_assert(cs.header->nr_snips_alloc >= CS_SNIP_ALLOC_BATCH);

    return true;
}

static bool test__cs_trim__no_remove_when_still_referenced(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    uint64_t hash;
    for (size_t i = 0; i < 2; i++) {
        int ret = cs_add(&cs, "test content", &hash, CS_DUPE_KEEP_ALL);
        t_assert(ret == 0);
    }

    _drop_(cs_content_unmap) struct cs_content content;

    t_assert(cs.header->nr_snips == 2);
    t_assert(cs_content_get(&cs, hash, &content) == 0);

    t_assert(cs_trim(&cs, CS_ITER_NEWEST_FIRST, 1) == 0);
    t_assert(cs.header->nr_snips == 1);
    t_assert(cs_content_get(&cs, hash, &content) == 0);

    t_assert(cs_trim(&cs, CS_ITER_NEWEST_FIRST, 0) == 0);
    t_assert(cs.header->nr_snips == 0);
    t_assert(cs_content_get(&cs, hash, &content) == -ENOENT);

    return true;
}

static bool test__cs_replace__out_of_bounds(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    add_ten_snips(&cs);

    int ret = cs_replace(&cs, CS_ITER_NEWEST_FIRST, 10, "test content", NULL);
    t_assert(ret == -ERANGE);

    return true;
}

static bool test__cs_snip__correct_nr_lines(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    add_ten_snips(&cs);

    /* No need to do exhaustive ones, they're done in test__first_line_* */
    uint64_t hash;
    struct cs_snip *snip = NULL;
    int ret =
        cs_replace(&cs, CS_ITER_NEWEST_FIRST, 0, "one\ntwo\nthree", &hash);

    t_assert(ret == 0);
    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);
    t_assert(cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip));
    t_assert(snip->hash == hash);
    t_assert(snip->nr_lines == 3);

    return true;
}

static bool test__first_line__empty(void) {
    char line[CS_SNIP_LINE_SIZE];
    size_t num_lines = first_line("", line);
    printf("%s\n", line);
    t_assert(streq(line, ""));
    t_assert(num_lines == 0);
    num_lines = first_line("\n", line);
    t_assert(streq(line, ""));
    t_assert(num_lines == 1);

    num_lines = first_line("\n\n\n", line);
    t_assert(streq(line, ""));
    t_assert(num_lines == 3);
    return true;
}

static bool test__first_line__only_one(void) {
    char line[CS_SNIP_LINE_SIZE];
    size_t num_lines = first_line("Foo bar\n", line);
    t_assert(streq(line, "Foo bar"));
    t_assert(num_lines == 1);
    return true;
}

static bool test__first_line__multiple(void) {
    char line[CS_SNIP_LINE_SIZE];
    size_t num_lines = first_line("Foo bar\nbaz\nqux\n", line);
    t_assert(streq(line, "Foo bar"));
    t_assert(num_lines == 3);
    return true;
}

static bool test__first_line__no_final_newline(void) {
    /* If the last line didn't end with a newline, still count it */
    char line[CS_SNIP_LINE_SIZE];
    size_t num_lines = first_line("Foo bar", line);
    t_assert(streq(line, "Foo bar"));
    t_assert(num_lines == 1);
    num_lines = first_line("Foo bar\nbaz", line);
    t_assert(streq(line, "Foo bar"));
    t_assert(num_lines == 2);
    return true;
}

static bool test__first_line__ignore_blank_lines(void) {
    char line[CS_SNIP_LINE_SIZE];
    size_t num_lines = first_line("\n\n\nFoo bar\n\n\n", line);
    t_assert(streq(line, "Foo bar"));
    t_assert(num_lines == 6);
    return true;
}

static bool test__first_line__unicode(void) {
    char line[CS_SNIP_LINE_SIZE];
    size_t num_lines = first_line("道", line);
    t_assert(streq(line, "道"));
    t_assert(num_lines == 1);
    num_lines = first_line("道\n", line);
    t_assert(streq(line, "道"));
    t_assert(num_lines == 1);
    num_lines = first_line("道\n非", line);
    t_assert(streq(line, "道"));
    t_assert(num_lines == 2);
    return true;
}

static bool test__synchronisation(void) {
    _drop_(remove_test_snip_fd) int snip_fd1 = create_test_snip_fd();
    _drop_(remove_test_content_dir_fd) int content_dir_fd1 =
        create_test_content_dir_fd();
    _drop_(close) int snip_fd2 = dup(snip_fd1);
    _drop_(close) int content_dir_fd2 = dup(content_dir_fd1);

    assert(snip_fd2 >= 0 && content_dir_fd2 >= 0);

    struct clip_store cs1, cs2;
    int ret = cs_init(&cs1, snip_fd1, content_dir_fd1);
    t_assert(ret == 0);
    ret = cs_init(&cs2, snip_fd2, content_dir_fd2);
    t_assert(ret == 0);

    uint64_t hash;
    ret = cs_add(&cs1, "test content", &hash, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);

    bool found = false;
    struct cs_snip *snip = NULL;
    _drop_(cs_unref) struct ref_guard guard_cs2 = cs_ref(&cs2);
    while (cs_snip_iter(&guard_cs2, CS_ITER_NEWEST_FIRST, &snip)) {
        if (snip->hash == hash) {
            found = true;
            break;
        }
    }
    t_assert(found);

    ret = cs_trim(&cs2, CS_ITER_NEWEST_FIRST, 0);
    t_assert(ret == 0);

    found = false;
    snip = NULL;
    _drop_(cs_unref) struct ref_guard guard_cs1 = cs_ref(&cs1);
    while (cs_snip_iter(&guard_cs1, CS_ITER_NEWEST_FIRST, &snip)) {
        found = true;
    }
    t_assert(!found);

    t_assert(cs_destroy(&cs1) == 0);
    t_assert(cs_destroy(&cs2) == 0);

    return true;
}

static bool test__cs_add__dupe_keep_all(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    uint64_t hash1, hash2;
    int ret = cs_add(&cs, "duplicate", &hash1, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    ret = cs_add(&cs, "duplicate", &hash2, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    t_assert(hash1 == hash2);
    t_assert(cs.header->nr_snips == 2);

    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);
    struct cs_snip *snip = NULL;
    bool iter_ret = cs_snip_iter(&guard, CS_ITER_OLDEST_FIRST, &snip);
    t_assert(iter_ret == true);
    t_assert(snip->hash == hash1);
    iter_ret = cs_snip_iter(&guard, CS_ITER_OLDEST_FIRST, &snip);
    t_assert(iter_ret == true);
    t_assert(snip->hash == hash2);

    return true;
}

static bool test__cs_add__dupe_keep_last(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    uint64_t hash1, hash2, hash3;
    int ret = cs_add(&cs, "duplicate", &hash1, CS_DUPE_KEEP_LAST);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 1);
    ret = cs_add(&cs, "duplicate", &hash2, CS_DUPE_KEEP_LAST);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 1);
    ret = cs_add(&cs, "duplicate", &hash3, CS_DUPE_KEEP_LAST);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 1);
    t_assert(hash1 == hash2);
    t_assert(hash1 == hash3);

    return true;
}

/* After adding a duplicate entry, ensure the duplicate is moved to the newest
 * slot while other entries remain in order. */
static bool test__cs_add__dupe_keep_last_with_multiple_entries(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    uint64_t hash_a, hash_dup;
    int ret = cs_add(&cs, "A", &hash_a, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    ret = cs_add(&cs, "duplicate", &hash_dup, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    ret = cs_add(&cs, "B", NULL, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 3);
    /* Now add a duplicate entry with KEEP_LAST which should move the duplicate
     * to the newest slot */
    ret = cs_add(&cs, "duplicate", NULL, CS_DUPE_KEEP_LAST);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 3);
    _drop_(cs_unref) struct ref_guard guard = cs_ref(&cs);
    struct cs_snip *snip = NULL;
    bool iter_ret = cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip);
    t_assert(iter_ret == true);
    t_assert(snip->hash == hash_dup);

    return true;
}

static bool check_order(struct clip_store *cs, uint64_t *order, size_t len) {
    _drop_(cs_unref) struct ref_guard guard = cs_ref(cs);
    struct cs_snip *snip = NULL;
    for (size_t i = 0; i < len; ++i) {
        bool iter_ret = cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip);
        t_assert(iter_ret == true);
        t_assert(snip->hash == order[i]);
    }
    return 0;
}

/* After calling cs_make_newest make sure the entry is at the newest slot while
 * other entries remain in order. */
static bool test__cs_make_newest(void) {
    _drop_(teardown_test) struct clip_store cs = setup_test();

    uint64_t hash_a, hash_b, hash_c;
    int ret = cs_add(&cs, "A", &hash_a, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    ret = cs_add(&cs, "B", &hash_b, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    ret = cs_add(&cs, "C", &hash_c, CS_DUPE_KEEP_ALL);
    t_assert(ret == 0);
    t_assert(cs.header->nr_snips == 3);

    uint64_t order_before[3] = { hash_c, hash_b, hash_a };
    ret = check_order(&cs, order_before, 3);
    t_assert(ret == 0);
    /* Now the order should change to ["A", "C", "B"] */
    ret = cs_make_newest(&cs, hash_a);
    t_assert(ret == 0);
    uint64_t order_after[3] = { hash_a, hash_c, hash_b };
    ret = check_order(&cs, order_after, 3);
    t_assert(ret == 0);

    return true;
}

int main(void) {
    t_run(test__cs_init);
    t_run(test__cs_init__bad_size);
    t_run(test__cs_init__bad_size_aligned);
    t_run(test__cs_add);
    t_run(test__cs_snip_iter);
    t_run(test__cs_remove);
    t_run(test__cs_trim);
    t_run(test__cs_replace);
    t_run(test__reuse_cs);
    t_run(test__cs_add__exceeds_snip_line_size);
    t_run(test__cs_trim__when_empty);
    t_run(test__cs_remove___empty);
    t_run(test__cs_add__around_alloc_batch_threshold);
    t_run(test__cs_replace__out_of_bounds);
    t_run(test__synchronisation);
    t_run(test__cs_trim__no_remove_when_still_referenced);
    t_run(test__cs_snip__correct_nr_lines);
    t_run(test__first_line__empty);
    t_run(test__first_line__only_one);
    t_run(test__first_line__multiple);
    t_run(test__first_line__no_final_newline);
    t_run(test__first_line__ignore_blank_lines);
    t_run(test__first_line__unicode);
    t_run(test__cs_add__dupe_keep_all);
    t_run(test__cs_add__dupe_keep_last);
    t_run(test__cs_add__dupe_keep_last_with_multiple_entries);
    t_run(test__cs_make_newest);

    return 0;
}
