#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
/* "[N] " prefix + snip line + " (N lines)\n" suffix + NUL */
#define LAUNCHER_LINE_MAX                                                      \
    (1 + UINT64_MAX_STRLEN + 2 + (CS_SNIP_LINE_SIZE - 1) + 2 +                 \
     UINT64_MAX_STRLEN + 7 + 1 + 1)

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

static int parse_sel_idx(const char *line, size_t cur_clips, size_t *out_idx) {
    if (line[0] != '[') {
        return -1;
    }
    const char *start = line + 1;
    const char *end = strchr(start, ']');
    if (!end) {
        return -1;
    }
    char idx_str[UINT64_MAX_STRLEN + 1];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(idx_str)) {
        return -1;
    }
    memcpy(idx_str, start, len);
    idx_str[len] = '\0';
    uint64_t sel_idx;
    if (str_to_uint64(idx_str, &sel_idx) < 0 || sel_idx == 0 ||
        sel_idx > cur_clips) {
        return -1;
    }
    *out_idx = (size_t)(sel_idx - 1);
    return 0;
}

/**
 * Writes the available clips to the launcher and reads back the user's
 * selection(s), calling action for each selected clip.
 */
static int _nonnull_ interact_with_dmenu(struct config *cfg, int *input_pipe,
                                         int *output_pipe, pid_t launcher_pid,
                                         clip_action_fn action) {
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

        char prefix[64];
        size_t prefix_len =
            snprintf_safe(prefix, sizeof(prefix), "[%*zu] ", pad, idx + 1);
        int write_ret = launcher_write_all(input_pipe[1], prefix, prefix_len);
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
            char lines_suffix[64];
            size_t lines_suffix_len =
                snprintf_safe(lines_suffix, sizeof(lines_suffix),
                              " (%zu lines)", snip->nr_lines);
            write_ret = launcher_write_all(input_pipe[1], lines_suffix,
                                           lines_suffix_len);
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

    _drop_(fclose) FILE *output = fdopen(output_pipe[0], "r");
    expect(output != NULL);
    char line[LAUNCHER_LINE_MAX];
    while (!forced_ret && fgets(line, sizeof(line), output) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        if (len == 0) {
            continue;
        }
        size_t idx;
        if (parse_sel_idx(line, cur_clips, &idx) == 0) {
            int ret = action(cfg, idx_to_hash[idx]);
            if (ret != 0) {
                forced_ret = ret;
            }
        } else {
            forced_ret = EXIT_FAILURE;
        }
    }

    int dmenu_status;
    int wait_ret = wait_for_pid(launcher_pid, &dmenu_status);

    if (forced_ret || wait_ret < 0 || !WIFEXITED(dmenu_status)) {
        return EXIT_FAILURE;
    }

    return WEXITSTATUS(dmenu_status);
}

/**
 * Prompts the user to select clip(s) via their configured launcher, and
 * executes the provided action on each selected clip.
 */
int _nonnull_ menu_prompt_and_act(struct config *cfg, const char *prompt,
                                  clip_action_fn action) {
    int input_pipe[2], output_pipe[2];
    expect(pipe(input_pipe) == 0 && pipe(output_pipe) == 0);

    pid_t pid = fork();
    expect(pid >= 0);

    if (pid == 0) {
        exec_launcher(cfg, prompt, input_pipe, output_pipe);
    }

    return interact_with_dmenu(cfg, input_pipe, output_pipe, pid, action);
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
