#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"
#include "menu_util.h"
#include "store.h"
#include "util.h"
#include "x.h"

static int _nonnull_ clipmenu_action(struct config *cfg, uint64_t hash) {
    if (cfg->touch_on_select) {
        _drop_(close) int content_dir_fd = open(get_cache_dir(cfg), O_RDONLY);
        _drop_(close) int snip_fd =
            open(get_line_cache_path(cfg), O_RDWR | O_CREAT, 0600);
        expect(content_dir_fd >= 0 && snip_fd >= 0);

        _drop_(cs_destroy) struct clip_store cs;
        expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);
        expect(cs_make_newest(&cs, hash) == 0);
    }

    run_clipserve(hash, NULL);
    return 0;
}

int main(int argc, char *argv[]) {
    menu_set_user_args(argc, argv);

    _drop_(config_free) struct config cfg = setup("clipmenu");
    exec_man_on_help(argc, argv);

    return menu_prompt_and_act(&cfg, "clipmenu", clipmenu_action);
}
