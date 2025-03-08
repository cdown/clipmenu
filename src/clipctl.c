#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
static pid_t get_clipmenud_pid(void) {
    _drop_(closedir) DIR *dir = opendir("/proc");
    die_on(!dir, "Support without /proc is not implemented yet\n");

    pid_t ret = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) && ret >= 0) {
        uint64_t pid;
        if (str_to_uint64(ent->d_name, &pid) < 0) {
            continue;
        }
        char buf[PATH_MAX];
        snprintf_safe(buf, sizeof(buf), "/proc/%s/comm", ent->d_name);
        _drop_(fclose) FILE *fp = fopen(buf, "r");
        if (fp && fgets(buf, sizeof(buf), fp) && streq(buf, "clipmenud\n")) {
            ret = ret ? -EEXIST : (pid_t)pid;
        }
    }

    return ret ? ret : -ENOENT;
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
    die_on(argc != 2, "Usage: clipctl <enable|disable|toggle|status>\n");

    pid_t pid = get_clipmenud_pid();
    die_on(pid == -ENOENT, "clipmenud is not running\n");
    die_on(pid == -EEXIST, "Multiple instances of clipmenud are running\n");
    expect(pid > 0);

    if (streq(argv[1], "status")) {
        printf("%s\n", is_enabled(&cfg) ? "enabled" : "disabled");
        return 0;
    }

    bool want_enable = should_enable(&cfg, argv[1]);

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

    die("Failed to %s clipmenud within %u ms\n",
        want_enable ? "enable" : "disable", max_wait_ms);
}
