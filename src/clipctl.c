#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

/**
 * Check if the clipmenud service is enabled.
 */
static bool is_enabled(struct config *cfg) {
    _drop_(fclose) FILE *file = fopen(get_enabled_path(cfg), "r");
    die_on(!file, "Failed to open enabled file: %s\n", strerror(errno));
    return fgetc(file) == '1';
}

/**
 * Retrieve the process ID of the clipmenud daemon.
 *
 * -EEXIST is returned if multiple instances are detected. -ENOENT is returned
 * if no instances are found.
 */
static pid_t get_clipmenud_pid(struct config *cfg) {
    int fd = open(get_session_lock_path(cfg), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -ENOENT;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        close(fd);
        return -ENOENT;
    }
    bool would_block;
#if EWOULDBLOCK == EAGAIN
    would_block = errno == EWOULDBLOCK;
#else
    would_block = errno == EWOULDBLOCK || errno == EAGAIN;
#endif
    if (!would_block) {
        close(fd);
        return -ENOENT;
    }

    char buf[32];
    ssize_t read_sz = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (read_sz <= 0) {
        return -ENOENT;
    }
    buf[read_sz] = '\0';
    char *newline = strchr(buf, '\n');
    if (newline) {
        *newline = '\0';
    }

    uint64_t pid;
    if (str_to_uint64(buf, &pid) < 0) {
        return -ENOENT;
    }

    if (kill((pid_t)pid, 0) < 0 && errno == ESRCH) {
        return -ENOENT;
    }

    return (pid_t)pid;
}

/**
 * Determine if clipmenud should be enabled based on the given mode string and
 * clipmenu state.
 */
static bool _nonnull_ should_enable(struct config *cfg, const char *mode_str) {
    if (streq(mode_str, "enable")) {
        return true;
    } else if (streq(mode_str, "disable")) {
        return false;
    } else if (streq(mode_str, "toggle")) {
        return !is_enabled(cfg);
    }

    die("Unknown command: %s\n", mode_str);
}

int main(int argc, char *argv[]) {
    _drop_(config_free) struct config cfg = setup("clipctl");
    exec_man_on_help(argc, argv);
    die_on(argc != 2,
           "Usage: clipctl <enable|disable|toggle|status|version|cache-dir>\n");

    const char *cmd = argv[1];

    if (streq(cmd, "cache-dir")) {
        printf("%s\n", get_cache_dir(&cfg));
        return 0;
    }
    if (streq(cmd, "version")) {
        printf("%d\n", CLIPMENU_VERSION);
        return 0;
    }

    pid_t pid = get_clipmenud_pid(&cfg);
    die_on(pid == -ENOENT, "clipmenud is not running\n");
    expect(pid > 0);

    if (streq(cmd, "status")) {
        printf("%s\n", is_enabled(&cfg) ? "enabled" : "disabled");
        return 0;
    }

    bool want_enable = should_enable(&cfg, cmd);

    expect(kill(pid, want_enable ? SIGUSR2 : SIGUSR1) == 0);
    dbg("Sent signal to pid %d\n", pid);

    unsigned int delay_ms = 1;
    unsigned int total_wait_ms = 0;
    const unsigned int max_wait_ms = 1000;

    while (total_wait_ms < max_wait_ms) {
        if (is_enabled(&cfg) == want_enable) {
            return 0;
        }

        struct timespec req = {.tv_sec = delay_ms / 1000,
                               .tv_nsec = (delay_ms % 1000) * 1000000L};
        struct timespec rem;

        while (nanosleep(&req, &rem) != 0) {
            expect(errno == EINTR);
            req = rem;
        }

        total_wait_ms += delay_ms;
        delay_ms *= 2;
    }

    if (is_enabled(&cfg) == want_enable) {
        return 0;
    }

    die("Failed to %s clipmenud within %u ms\n",
        want_enable ? "enable" : "disable", max_wait_ms);
}
