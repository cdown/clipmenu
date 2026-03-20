#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "menu_util.h"
#include "store.h"
#include "util.h"

#define MAX_ARGS 32

static int dmenu_user_argc;
static char **dmenu_user_argv;

/**
 * Calculate the base 10 padding length for a number.
 */
static int get_padding_length(size_t num) {
    int digits = 1;
    while (num /= 10) {
        digits++;
    }
    return digits;
}

/**
 * Execute the launcher. Called after fork() is already done in the new child.
 */
static void _noreturn_ _nonnull_ exec_launcher(struct config *cfg,
                                               const char *prompt,
                                               int *input_pipe,
                                               int *output_pipe) {
    dup2(input_pipe[0], STDIN_FILENO);
    close(input_pipe[1]);
    close(output_pipe[0]);
    dup2(output_pipe[1], STDOUT_FILENO);

    const char *dmenu_args[] = {"-p", prompt, "-l", "20"};
    const char **cmd = malloc(MAX_ARGS * sizeof(char *));

    size_t d_i = 0;

    switch (cfg->launcher.ltype) {
        case LAUNCHER_ROFI:
            cmd[d_i++] = "rofi";
            cmd[d_i++] = "--";
            cmd[d_i++] = "-dmenu";
            break;
        case LAUNCHER_CUSTOM:
            cmd[d_i++] = cfg->launcher.custom;
            break;
        default:
            die("Unreachable\n");
    }

    if (cfg->launcher_pass_dmenu_args) {
        expect(d_i + arrlen(dmenu_args) < MAX_ARGS);
        for (size_t i = 0; i < arrlen(dmenu_args); i++) {
            cmd[d_i++] = dmenu_args[i];
        }
    }

    for (int i = 1; i < dmenu_user_argc && d_i < MAX_ARGS - 1; i++, d_i++) {
        cmd[d_i] = dmenu_user_argv[i];
    }

    cmd[d_i] = NULL;
    execvp(cmd[0], (char *const *)cmd); // SUS says cmd unchanged
    die("Failed to exec %s: %s\n", cmd[0], strerror(errno));
}

static int dprintf_ellipsise_long_snip_line(int fd, const char *line) {
    size_t line_len = strlen(line);
    if (line_len == CS_SNIP_LINE_SIZE - 1) {
        return dprintf(fd, "%.*s...", (int)(CS_SNIP_LINE_SIZE - 4), line);
    } else {
        return dprintf(fd, "%s", line);
    }
}

static int launcher_vdprintf(int fd, const char *fmt, va_list args) {
    if (vdprintf(fd, fmt, args) < 0) {
        return negative_errno();
    }
    return 0;
}

static int launcher_dprintf(int fd, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = launcher_vdprintf(fd, fmt, args);
    va_end(args);
    return ret;
}

static int launcher_write_all(int fd, const char *buf, size_t count) {
    while (count > 0) {
        ssize_t written = write(fd, buf, count);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return negative_errno();
        }
        buf += written;
        count -= (size_t)written;
    }
    return 0;
}

static int wait_for_pid(pid_t pid, int *status) {
    while (waitpid(pid, status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return negative_errno();
    }
    return 0;
}

/**
 * Writes the available clips to the launcher and reads back the user's
 * selection.
 */
static int _nonnull_ interact_with_dmenu(struct config *cfg, int *input_pipe,
                                         int *output_pipe, pid_t launcher_pid,
                                         uint64_t *out_hash) {
    close(input_pipe[0]);
    close(output_pipe[1]);

    _drop_(close) int content_dir_fd = open(get_cache_dir(cfg), O_RDONLY);
    _drop_(close) int snip_fd =
        open(get_line_cache_path(cfg), O_RDWR | O_CREAT, 0600);
    expect(content_dir_fd >= 0 && snip_fd >= 0);

    _drop_(cs_destroy) struct clip_store cs;
    expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);

    struct ref_guard guard = cs_ref(&cs);
    size_t cur_clips;
    expect(cs_len(&cs, &cur_clips) == 0);
    _drop_(free) uint64_t *idx_to_hash = malloc(cur_clips * sizeof(uint64_t));
    expect(idx_to_hash);
    int pad = get_padding_length(cur_clips);
    size_t clip_idx = cur_clips;
    bool launcher_closed_stdin = false;
    int forced_ret = 0;

    struct sigaction ignore_sa = {0};
    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    struct sigaction old_sa;
    expect(sigaction(SIGPIPE, &ignore_sa, &old_sa) == 0);

    struct cs_snip *snip = NULL;
    while (cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip)) {
        size_t idx = --clip_idx;
        idx_to_hash[idx] = snip->hash;

        if (launcher_closed_stdin) {
            continue;
        }

        int write_ret =
            launcher_dprintf(input_pipe[1], "[%*zu] ", pad, idx + 1);
        if (write_ret == -EPIPE) {
            launcher_closed_stdin = true;
            continue;
        }
        if (write_ret < 0) {
            forced_ret = EXIT_FAILURE;
            break;
        }

        write_ret = dprintf_ellipsise_long_snip_line(input_pipe[1], snip->line);
        if (write_ret < 0 && errno == EPIPE) {
            launcher_closed_stdin = true;
            continue;
        }
        if (write_ret < 0) {
            forced_ret = EXIT_FAILURE;
            break;
        }

        if (snip->nr_lines > 1) {
            write_ret =
                launcher_dprintf(input_pipe[1], " (%zu lines)", snip->nr_lines);
            if (write_ret == -EPIPE) {
                launcher_closed_stdin = true;
                continue;
            }
            if (write_ret < 0) {
                forced_ret = EXIT_FAILURE;
                break;
            }
        }

        write_ret = launcher_write_all(input_pipe[1], "\n", 1);
        if (write_ret == -EPIPE) {
            launcher_closed_stdin = true;
            continue;
        }
        if (write_ret < 0) {
            forced_ret = EXIT_FAILURE;
            break;
        }
    }

    // We've written everything and have our own map, no need to hold any more
    cs_unref(guard.cs);

    close(input_pipe[1]);
    expect(sigaction(SIGPIPE, &old_sa, NULL) == 0);

    char sel_idx_str[UINT64_MAX_STRLEN + 1];
    read_safe(output_pipe[0], sel_idx_str, 1); // Discard the leading "["
    size_t read_sz = read_safe(output_pipe[0], sel_idx_str, UINT64_MAX_STRLEN);
    sel_idx_str[read_sz] = '\0';
    char *end_ptr = strchr(sel_idx_str, ']');
    if (end_ptr) {
        *end_ptr = '\0';
    }

    uint64_t sel_idx;
    if (str_to_uint64(sel_idx_str, &sel_idx) < 0 || sel_idx == 0 ||
        sel_idx > cur_clips) {
        forced_ret = EXIT_FAILURE;
    } else {
        *out_hash = idx_to_hash[sel_idx - 1];
    }

    int dmenu_status;
    int wait_ret = wait_for_pid(launcher_pid, &dmenu_status);
    close(output_pipe[0]);

    if (forced_ret || wait_ret < 0 || !WIFEXITED(dmenu_status)) {
        return EXIT_FAILURE;
    }

    return WEXITSTATUS(dmenu_status);
}

/**
 * Prompts the user to select a clip via their launcher, and executes
 * the provided action on the selected clip.
 */
static int _nonnull_ prompt_user_for_hash(struct config *cfg,
                                          const char *prompt, uint64_t *hash) {
    int input_pipe[2], output_pipe[2];
    expect(pipe(input_pipe) == 0 && pipe(output_pipe) == 0);

    pid_t pid = fork();
    expect(pid >= 0);

    if (pid == 0) {
        exec_launcher(cfg, prompt, input_pipe, output_pipe);
    }

    return interact_with_dmenu(cfg, input_pipe, output_pipe, pid, hash);
}

/**
 * Prompts the user to select a clip via their configured launcher, and
 * executes the provided action on the selected clip.
 */
int _nonnull_ menu_prompt_and_act(struct config *cfg, const char *prompt,
                                  clip_action_fn action) {
    uint64_t hash;
    int dmenu_exit_code = prompt_user_for_hash(cfg, prompt, &hash);

    if (dmenu_exit_code == EXIT_SUCCESS) {
        return action(cfg, hash);
    }

    return dmenu_exit_code;
}

/**
 * Set the user-provided command-line arguments for launcher passthrough.
 *
 * This is janky and just mimics how clipmenu worked by itself, in the long run
 * we should not rely on these.
 */
void menu_set_user_args(int argc, char **argv) {
    dmenu_user_argc = argc;
    dmenu_user_argv = argv;
}
