#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"
#include "menu_util.h"
#include "store.h"
#include "util.h"

struct delete_state {
    uint64_t hash_to_delete;
};

// cppcheck-suppress-begin constParameterCallback
static enum cs_remove_action _nonnull_
remove_if_hash_match(uint64_t hash, const char *line _unused_, void *private) {
    const struct delete_state *state = private;
    return hash == state->hash_to_delete ? CS_ACTION_REMOVE : CS_ACTION_KEEP;
}
// cppcheck-suppress-end constParameterCallback

static int _nonnull_ clipdelmenu_action(struct config *cfg, uint64_t hash) {
    _drop_(close) int content_dir_fd = open(get_cache_dir(cfg), O_RDONLY);
    _drop_(close) int snip_fd =
        open(get_line_cache_path(cfg), O_RDWR | O_CREAT, 0600);
    expect(content_dir_fd >= 0 && snip_fd >= 0);

    _drop_(cs_destroy) struct clip_store cs;
    expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);

    struct delete_state del_state = {.hash_to_delete = hash};
    expect(cs_remove(&cs, CS_ITER_NEWEST_FIRST, remove_if_hash_match,
                     &del_state) == 0);

    return 0;
}

int main(int argc, char *argv[]) {
    menu_set_user_args(argc, argv);

    _drop_(config_free) struct config cfg = setup("clipdelmenu");
    exec_man_on_help(argc, argv);

    return menu_prompt_and_act(&cfg, "clipdelmenu", clipdelmenu_action);
}
