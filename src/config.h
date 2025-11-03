#ifndef CM_CONFIG_H
#define CM_CONFIG_H

#include <X11/Xlib.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>

#include "util.h"

struct selection {
    const char *name;
    bool active;
    Atom *atom;
};
enum selection_type {
    CM_SEL_CLIPBOARD,
    CM_SEL_PRIMARY,
    CM_SEL_SECONDARY,
    CM_SEL_MAX,
    CM_SEL_INVALID
};
struct cm_selections {
    Atom selection;
    Atom storage;
};
struct ignore_window {
    bool set;
    regex_t rgx;
};
enum launcher_known {
    LAUNCHER_ROFI,
    LAUNCHER_CUSTOM,
};
struct launcher {
    enum launcher_known ltype;
    char *custom;
};
struct config {
    bool ready;
    bool debug;
    char *runtime_dir;
    int max_clips;
    int max_clips_batch;
    int oneshot;
    bool deduplicate;
    bool own_clipboard;
    struct selection *owned_selections;
    struct selection *selections;
    struct ignore_window ignore_window;
    struct launcher launcher;
    bool launcher_pass_dmenu_args;
    bool touch_on_select;
    int partial_merge_secs;
};
typedef int (*conversion_func_t)(const char *, void *);
struct config_entry {
    const char *config_key;
    const char *env_var;
    void *value;
    conversion_func_t convert;
    const char *default_value;
    bool is_set;
};

char *get_cache_dir(struct config *cfg);

void exec_man(void);
void exec_man_on_help(int argc, char *argv[]);

/**
 * Define a function that generates and caches a path within the application's
 * cache directory.
 *
 * For example, DEFINE_GET_PATH_FUNCTION(foo) defines a function `get_foo_path`
 * that returns the path to "foo" within the cache directory.
 */
#define DEFINE_GET_PATH_FUNCTION(name)                                         \
    static inline char *get_##name##_path(struct config *cfg) {                \
        static char path[PATH_MAX];                                            \
        /* Just in case the config changed, do the write anyway */             \
        snprintf_safe(path, PATH_MAX, "%s/" #name, get_cache_dir(cfg));        \
        return path;                                                           \
    }

DEFINE_GET_PATH_FUNCTION(line_cache)
DEFINE_GET_PATH_FUNCTION(enabled)
DEFINE_GET_PATH_FUNCTION(session_lock)

extern const char *prog_name;
struct config _nonnull_ setup(const char *inner_prog_name);
void _nonnull_ setup_selections(Display *dpy, struct cm_selections *sels);
enum selection_type _nonnull_
selection_atom_to_selection_type(Atom atom, const struct cm_selections *sels);
enum selection_type _nonnull_
storage_atom_to_selection_type(Atom atom, const struct cm_selections *sels);

int convert_bool(const char *str, void *output);
int convert_positive_int(const char *str, void *output);
int convert_ignore_window(const char *str, void *output);
int config_setup_internal(FILE *file, struct config *cfg);
void config_free(struct config *cfg);
DEFINE_DROP_FUNC_PTR(struct config, config_free)

#endif
