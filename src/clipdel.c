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
 * Holds the application state for a clipdel operation in preparation for
 * passing it as private data to the cs_remove callback.
 */
struct clipdel_state {
    enum delete_mode mode;
    bool invert_match;
    bool literal_match;
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
    bool matches;
    if (state->literal_match) {
        matches = strstr(line, state->needle) != NULL;
    } else {
        int ret = regexec(&state->rgx, line, 0, NULL, 0);
        expect(ret == 0 || ret == REG_NOMATCH);
        matches = ret == 0;
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

    struct clipdel_state state = {
        .mode = DELETE_DRY_RUN,
        .invert_match = false,
        .literal_match = false,
    };

    int opt;
    while ((opt = getopt(argc, argv, "dFvh")) != -1) {
        switch (opt) {
            case 'd':
                state.mode = DELETE_REAL;
                break;
            case 'F':
                state.literal_match = true;
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

    if (!state.literal_match) {
        die_on(regcomp(&state.rgx, argv[optind], REG_EXTENDED | REG_NOSUB),
               "Could not compile regex\n");
    } else {
        state.needle = argv[optind];
    }

    expect(cs_remove(&cs, CS_ITER_OLDEST_FIRST, remove_if_match, &state) == 0);

    if (!state.literal_match) {
        regfree(&state.rgx);
    }

    return 0;
}
