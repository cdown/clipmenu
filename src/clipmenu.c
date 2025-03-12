#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
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
                                               int *input_pipe,
                                               int *output_pipe) {
    dup2(input_pipe[0], STDIN_FILENO);
    close(input_pipe[1]);
    close(output_pipe[0]);
    dup2(output_pipe[1], STDOUT_FILENO);

    const char *const dmenu_args[] = {"-p", "clipmenu", "-l", "20"};
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

/**
 * Writes the available clips to the launcher and reads back the user's
 * selection.
 */
static int _nonnull_ interact_with_dmenu(struct config *cfg, int *input_pipe,
                                         int *output_pipe, uint64_t *out_hash) {
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

    struct cs_snip *snip = NULL;
    while (cs_snip_iter(&guard, CS_ITER_NEWEST_FIRST, &snip)) {
        expect(dprintf(input_pipe[1], "[%*zu] ", pad, clip_idx--) > 0);
        expect(dprintf_ellipsise_long_snip_line(input_pipe[1], snip->line) > 0);
        if (snip->nr_lines > 1) {
            expect(dprintf(input_pipe[1], " (%zu lines)", snip->nr_lines) > 0);
        }
        write_safe(input_pipe[1], "\n", 1);
        idx_to_hash[clip_idx] = snip->hash;
    }

    // We've written everything and have our own map, no need to hold any more
    cs_unref(guard.cs);

    close(input_pipe[1]);

    char sel_idx_str[UINT64_MAX_STRLEN + 1];
    read_safe(output_pipe[0], sel_idx_str, 1); // Discard the leading "["
    size_t read_sz = read_safe(output_pipe[0], sel_idx_str, UINT64_MAX_STRLEN);
    sel_idx_str[read_sz] = '\0';
    char *end_ptr = strchr(sel_idx_str, ']');
    if (end_ptr) {
        *end_ptr = '\0';
    }

    uint64_t sel_idx;
    int forced_ret = 0;
    if (str_to_uint64(sel_idx_str, &sel_idx) < 0 || sel_idx == 0 ||
        sel_idx > cur_clips) {
        forced_ret = EXIT_FAILURE;
    } else {
        *out_hash = idx_to_hash[sel_idx - 1];
    }

    int dmenu_status;
    while (wait(&dmenu_status) < 0 && errno == EINTR)
        ;
    close(output_pipe[0]);

    if (forced_ret || !WIFEXITED(dmenu_status)) {
        return EXIT_FAILURE;
    }

    int dmenu_exit_code = WEXITSTATUS(dmenu_status);
    if (dmenu_exit_code == EXIT_SUCCESS && cfg->touch_on_select) {
        expect(cs_make_newest(&cs, *out_hash) == 0);
    }
    return dmenu_exit_code;
}

/**
 * Prompts the user to select a clip via their launcher, and returns the
 * selected content hash.
 */
static int _nonnull_ prompt_user_for_hash(struct config *cfg, uint64_t *hash) {
    int input_pipe[2], output_pipe[2];
    expect(pipe(input_pipe) == 0 && pipe(output_pipe) == 0);

    pid_t pid = fork();
    expect(pid >= 0);

    if (pid == 0) {
        exec_launcher(cfg, input_pipe, output_pipe);
    }

    return interact_with_dmenu(cfg, input_pipe, output_pipe, hash);
}

int main(int argc, char *argv[]) {
    dmenu_user_argc = argc;
    dmenu_user_argv = argv;

    _drop_(config_free) struct config cfg = setup("clipmenu");
    exec_man_on_help(argc, argv);

    uint64_t hash;
    int dmenu_exit_code = prompt_user_for_hash(&cfg, &hash);

    if (dmenu_exit_code == EXIT_SUCCESS) {
        run_clipserve(hash);
    }

    return dmenu_exit_code;
}
