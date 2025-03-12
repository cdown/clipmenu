#include <fcntl.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "store.h"
#include "util.h"

/**
 * The deletion mode for clipdel operations.
 */
enum delete_mode {
    DELETE_DRY_RUN,
    DELETE_REAL,
};

/**
 * The match type for clipdel operations.
 */
enum match_type {
    MATCH_REGEX,
    MATCH_LITERAL,
};

/**
 * Holds the application state for a clipdel operation in preparation for
 * passing it as private data to the cs_remove callback.
 */
struct clipdel_state {
    enum delete_mode mode;
    enum match_type type;
    bool invert_match;
    union {
        regex_t rgx;
        const char *needle;
    };
};

/**
 * Callback for cs_remove. In order for the delete to actually happen, we must
 * be running DELETE_REAL.
 */
static enum cs_remove_action _nonnull_ remove_if_match(uint64_t hash _unused_,
                                                       const char *line,
                                                       void *private) {
    struct clipdel_state *state = private;
    int ret;
    bool matches;
    switch (state->type) {
        case MATCH_LITERAL:
            matches = strstr(line, state->needle) != NULL;
            break;
        case MATCH_REGEX:
            ret = regexec(&state->rgx, line, 0, NULL, 0);
            expect(ret == 0 || ret == REG_NOMATCH);
            matches = ret == 0;
            break;
        default:
            die("unreachable\n");
    }

    bool wants_del = state->invert_match ? !matches : matches;
    if (wants_del) {
        puts(line);
    }

    return state->mode == DELETE_REAL && wants_del ? CS_ACTION_REMOVE
                                                   : CS_ACTION_KEEP;
}

int main(int argc, char *argv[]) {
    const char usage[] = "Usage: clipdel [-d] [-F] [-v] pattern";

    _drop_(config_free) struct config cfg = setup("clipdel");

    struct clipdel_state state = {0};

    int opt;
    while ((opt = getopt(argc, argv, "dFvh")) != -1) {
        switch (opt) {
            case 'd':
                state.mode = DELETE_REAL;
                break;
            case 'F':
                state.type = MATCH_LITERAL;
                break;
            case 'v':
                state.invert_match = true;
                break;
            case 'h':
                exec_man();
                break;
            default:
                die("%s\n", usage);
        }
    }

    die_on(optind >= argc, "%s\n", usage);

    _drop_(close) int content_dir_fd = open(get_cache_dir(&cfg), O_RDONLY);
    _drop_(close) int snip_fd =
        open(get_line_cache_path(&cfg), O_RDWR | O_CREAT, 0600);
    expect(content_dir_fd >= 0 && snip_fd >= 0);

    _drop_(cs_destroy) struct clip_store cs;
    expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);

    switch (state.type) {
        case MATCH_REGEX:
            die_on(regcomp(&state.rgx, argv[optind], REG_EXTENDED | REG_NOSUB),
                   "Could not compile regex\n");
            break;
        case MATCH_LITERAL:
            state.needle = argv[optind];
            break;
        default:
            die("unreachable\n");
    }

    expect(cs_remove(&cs, CS_ITER_OLDEST_FIRST, remove_if_match, &state) == 0);

    if (state.type == MATCH_REGEX) {
        regfree(&state.rgx);
    }

    return 0;
}
