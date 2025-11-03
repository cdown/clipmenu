#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "x.h"

#define CLIPMENU_VERSION 7

/**
 * Determines the runtime directory for storing application data. This is _not_
 * the clip store, but the place to create the directory that will become it
 * (unless CM_DIR is set, in which case we use that directly).
 */
static const char *get_runtime_directory(void) {
    static const char *runtime_dir = NULL;
    const char *env_vars[] = {"XDG_RUNTIME_DIR", "TMPDIR"};

    if (runtime_dir) {
        return runtime_dir;
    }

    for (size_t i = 0; i < arrlen(env_vars); i++) {
        const char *dir = getenv(env_vars[i]);
        if (dir) {
            return (runtime_dir = dir);
        }
    }

    return (runtime_dir = "/tmp");
}

/**
 * Constructs the path to the clip store directory for clipmenu, creating the
 * directory if it does not exist.
 */
char *get_cache_dir(struct config *cfg) {
    expect(cfg->ready);
    static char cache_dir[PATH_MAX];
    // In case config changed, do the write anyway
    snprintf_safe(cache_dir, PATH_MAX, "%s/clipmenu.%d.%ld", cfg->runtime_dir,
                  CLIPMENU_VERSION, (long)getuid());
    die_on(mkdir(cache_dir, S_IRWXU) != 0 && errno != EEXIST,
           "Failed to create directory: %s\n", cache_dir);
    return cache_dir;
}

/**
 * This whole section consists of conversion functions to go from a string in
 * the config file to the type we expect for `struct Config`.
 */

int convert_bool(const char *str, void *output) {
    const char *const truthy[] = {"1", "y", "yes", "true", "on"};
    const char *const falsy[] = {"0", "n", "no", "false", "off"};

    for (size_t i = 0; i < arrlen(truthy); i++) {
        if (strceq(str, truthy[i])) {
            *(bool *)output = true;
            return 0;
        }
    }

    for (size_t i = 0; i < arrlen(falsy); i++) {
        if (strceq(str, falsy[i])) {
            *(bool *)output = false;
            return 0;
        }
    }

    return -EINVAL;
}

int convert_positive_int(const char *str, void *output) {
    char *end;
    long val = strtol(str, &end, 10);
    if (*end != '\0' || end == str || val < 0 || val > INT_MAX) {
        return -EINVAL;
    }
    *(int *)output = (int)val;
    return 0;
}

int convert_ignore_window(const char *str, void *output) {
    struct ignore_window *iw = output;
    iw->set = (bool)str;
    if (!iw->set) {
        return 0;
    }
    if (regcomp(&iw->rgx, str, REG_EXTENDED | REG_NOSUB)) {
        return -EINVAL;
    }
    return 0;
}

static int convert_cm_dir(const char *str, void *output) {
    if (!str) {
        str = get_runtime_directory();
    }
    char *rtd = strdup(str);
    expect(rtd);
    *(char **)output = rtd;
    return 0;
}

static int _nonnull_ convert_launcher(const char *str, void *output) {
    struct launcher *lnch = output;

    lnch->custom = strdup(str);
    expect(lnch->custom);

    if (streq(str, "rofi")) {
        lnch->ltype = LAUNCHER_ROFI;
    } else {
        lnch->ltype = LAUNCHER_CUSTOM;
    }

    return 0;
}

#define DEFAULT_SELECTION_STATE(name)                                          \
    (struct selection) { name, 0, NULL }

static int convert_selections(const char *str, void *output) {
    struct selection *sels = malloc(3 * sizeof(struct selection));
    expect(sels);
    sels[CM_SEL_CLIPBOARD] = DEFAULT_SELECTION_STATE("clipboard");
    sels[CM_SEL_PRIMARY] = DEFAULT_SELECTION_STATE("primary");
    sels[CM_SEL_SECONDARY] = DEFAULT_SELECTION_STATE("secondary");

    _drop_(free) char *inner_str = strdup(str);
    expect(inner_str);
    const char *token = strtok(inner_str, " ");
    size_t i;

    while (token) {
        bool found = false;
        for (i = 0; i < CM_SEL_MAX; i++) {
            if (streq(token, sels[i].name)) {
                sels[i].active = true;
                found = true;
                break;
            }
        }
        if (!found) {
            return -EINVAL;
        }
        token = strtok(NULL, " ");
    }

    *(struct selection **)output = sels;

    return 0;
}

/**
 * Constructs the path to the clipmenu configuration file. The user can
 * manually specify a path with $CM_CONFIG, otherwise, it's inferred based on
 * $XDG_CONFIG_HOME or ~/.config. It's typically
 * ~/.config/clipmenu/clipmenu.conf.
 */

static void get_config_file(char *config_path) {
    const char *cm_config = getenv("CM_CONFIG");
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (cm_config) {
        snprintf_safe(config_path, PATH_MAX, "%s", cm_config);
    } else if (xdg_config_home) {
        snprintf_safe(config_path, PATH_MAX, "%s/clipmenu/clipmenu.conf",
                      xdg_config_home);
    } else {
        die_on(!home,
               "None of $CM_CONFIG, $XDG_CONFIG_HOME, or $HOME is set\n");
        snprintf_safe(config_path, PATH_MAX,
                      "%s/.config/clipmenu/clipmenu.conf", home);
    }
}

static int config_parse_env_vars(struct config_entry entries[],
                                 size_t entries_len) {
    for (size_t i = 0; i < entries_len; ++i) {
        const char *env_var = entries[i].env_var;
        if (!env_var) {
            continue;
        }
        const char *env_value = getenv(env_var);
        if (env_value && entries[i].convert(env_value, entries[i].value) != 0) {
            fprintf(stderr, "Error parsing environment variable for $%s\n",
                    env_var);
            return -EINVAL;
        } else if (env_value) {
            dbg("Config entry %s is set to %s by $%s\n", entries[i].config_key,
                env_value, env_var);
            entries[i].is_set = true;
        }
    }

    return 0;
}

static int config_parse_file(FILE *file, struct config_entry entries[],
                             size_t entries_len) {
    if (!file) {
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        const char *key = strtok(line, " ");
        char *value = strtok(NULL, "\n");
        if (!key || !value) {
            continue;
        }
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        for (size_t i = 0; i < entries_len; ++i) {
            if (!entries[i].is_set && streq(entries[i].config_key, key)) {
                if (entries[i].convert(value, entries[i].value) != 0) {
                    fprintf(stderr, "Error parsing config file for %s\n",
                            entries[i].config_key);
                    return -EINVAL;
                }
                dbg("Config entry %s is set to %s by config file\n",
                    entries[i].config_key, value);
                entries[i].is_set = true;
                break;
            }
        }
    }

    return 0;
}

static int config_apply_default_values(struct config_entry entries[],
                                       size_t entries_len) {
    for (size_t i = 0; i < entries_len; ++i) {
        if (!entries[i].is_set) {
            if (entries[i].convert(entries[i].default_value,
                                   entries[i].value) != 0) {
                fprintf(stderr, "Error setting default value for %s\n",
                        entries[i].config_key);
                return -EINVAL;
            }
            dbg("Config entry %s is set to %s by fallback\n",
                entries[i].config_key, entries[i].default_value);
        }
    }
    return 0;
}

/**
 * Parse the clipmenu configuration file and environment variables to set up
 * the application configuration.
 *
 * Prior to version 7, clipmenu and friends could only be configured via
 * environment variables, so these are supported for backwards compatibility.
 * In general, it's more straightforward to use the config file nowadays.
 *
 * This is generally not expected to be called by applications -- call
 * config_setup() instead, which provides the right file for you.
 */
int config_setup_internal(FILE *file, struct config *cfg) {
    struct config_entry entries[] = {
        {"max_clips", "CM_MAX_CLIPS", &cfg->max_clips, convert_positive_int,
         "1000", 0},
        {"max_clips_batch", "CM_MAX_CLIPS_BATCH", &cfg->max_clips_batch,
         convert_positive_int, "100", 0},
        {"oneshot", "CM_ONESHOT", &cfg->oneshot, convert_positive_int, "0", 0},
        {"deduplicate", "CM_DEDUPLICATE", &cfg->deduplicate, convert_bool, "0",
         0},
        {"own_clipboard", "CM_OWN_CLIPBOARD", &cfg->own_clipboard, convert_bool,
         "0", 0},
        {"selections", "CM_SELECTIONS", &cfg->selections, convert_selections,
         "clipboard primary", 0},
        {"own_selections", "CM_OWN_SELECTIONS", &cfg->owned_selections,
         convert_selections, "clipboard", 0},
        {"ignore_window", "CM_IGNORE_WINDOW", &cfg->ignore_window,
         convert_ignore_window, NULL, 0},
        {"launcher", "CM_LAUNCHER", &cfg->launcher, convert_launcher, "dmenu",
         0},
        {"launcher_pass_dmenu_args", "CM_LAUNCHER_PASS_DMENU_ARGS",
         &cfg->launcher_pass_dmenu_args, convert_bool, "1", 0},
        {"touch_on_select", NULL, &cfg->touch_on_select, convert_bool, "0", 0},
        {"partial_merge_secs", NULL, &cfg->partial_merge_secs,
         convert_positive_int, "2", 0},
        {"cm_dir", "CM_DIR", &cfg->runtime_dir, convert_cm_dir, NULL, 0}};

    size_t entries_len = arrlen(entries);

    int ret = config_parse_env_vars(entries, entries_len);
    if (ret < 0) {
        return ret;
    }
    ret = config_parse_file(file, entries, entries_len);
    if (ret < 0) {
        return ret;
    }
    ret = config_apply_default_values(entries, entries_len);
    if (ret < 0) {
        return ret;
    }

    cfg->ready = true;

    return 0;
}

/**
 * Frees dynamically allocated memory within the config structure.
 */
void config_free(struct config *cfg) {
    free(cfg->runtime_dir);
    free(cfg->launcher.custom);
    free(cfg->selections);
    free(cfg->owned_selections);
    if (cfg->ignore_window.set) {
        regfree(&cfg->ignore_window.rgx);
    }
}

/**
 * Initialise the clipmenu configuration by loading settings from environment
 * variables and the configuration file.
 */
static void config_setup(struct config *cfg) {
    char config_path[PATH_MAX];
    get_config_file(config_path);
    _drop_(fclose) FILE *file = fopen(config_path, "r");
    expect(file || errno == ENOENT);
    die_on(config_setup_internal(file, cfg) != 0, "Invalid config\n");
}

static char stdout_buf[512];
const char *prog_name = "broken";

/**
 * Performs initial setup for clipmenu applications, including setting the
 * program name (used for dbg()), making sure stdout is line buffered, getting
 * the config, and setting up X11 error handling.
 */
struct config setup(const char *inner_prog_name) {
    struct config cfg;
    prog_name = inner_prog_name;
    expect(setvbuf(stdout, stdout_buf, _IOLBF, sizeof(stdout_buf)) == 0);
    config_setup(&cfg);
    XSetErrorHandler(xerror_handler);
    return cfg;
}

void setup_selections(Display *dpy, struct cm_selections *sels) {
    sels[CM_SEL_CLIPBOARD].selection = XInternAtom(dpy, "CLIPBOARD", False);
    sels[CM_SEL_CLIPBOARD].storage =
        XInternAtom(dpy, "CLIPMENUD_CUR_CLIPBOARD", False);
    sels[CM_SEL_PRIMARY].selection = XA_PRIMARY;
    sels[CM_SEL_PRIMARY].storage =
        XInternAtom(dpy, "CLIPMENUD_CUR_PRIMARY", False);
    sels[CM_SEL_SECONDARY].selection = XA_SECONDARY;
    sels[CM_SEL_SECONDARY].storage =
        XInternAtom(dpy, "CLIPMENUD_CUR_SECONDARY", False);
}

/**
 * Maps an Atom to a selection_type based on the selection atoms in the
 * provided cm_selections struct. Returns CM_SEL_INVALID on error if the
 * selection type is not found.
 */
enum selection_type
selection_atom_to_selection_type(Atom atom, const struct cm_selections *sels) {
    for (size_t i = 0; i < CM_SEL_MAX; ++i) {
        if (sels[i].selection == atom) {
            return i;
        }
    }
    return CM_SEL_INVALID;
}

/**
 * Maps an Atom to a selection_type based on the storage atoms in the provided
 * cm_selections struct. Returns CM_SEL_INVALID on error if the storage type is
 * not found.
 */
enum selection_type
storage_atom_to_selection_type(Atom atom, const struct cm_selections *sels) {
    for (size_t i = 0; i < CM_SEL_MAX; ++i) {
        if (sels[i].storage == atom) {
            return i;
        }
    }
    return CM_SEL_INVALID;
}

void exec_man(void) {
    execlp("man", "man", prog_name, NULL);
    die("Failed to exec man: %s\n", strerror(errno));
}

// cppcheck doesn't know this comes from an outside type
// cppcheck-suppress [constParameter,unmatchedSuppression]
void exec_man_on_help(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "--")) {
            break;
        }
        if (streq(argv[i], "-h") || streq(argv[i], "--help")) {
            exec_man();
        }
    }
}
