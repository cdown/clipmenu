#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "store.h"
#include "util.h"
#include "x.h"

static Display *dpy;
static struct clip_store cs;
static struct config cfg;
static Window win;

static int enabled = 1;
static int sig_fd;

static struct cm_selections sels[CM_SEL_MAX];

/**
 * Check if a text s1 is a possible partial of s2.
 *
 * Chromium and some other badly behaved applications spam PRIMARY during
 * selection, so if you're selecting the text "abc", you get three clips: "a",
 * "ab", and "abc" (or "c", "bc", "abc" if selecting right to left). Attempt to
 * detect these. It's possible we were not fast enough to get all of them, so
 * unfortunately we can't check for strlen(s1)+1 either. It's also possible the
 * user first expands, and then retracts the selection, so we need to handle
 * that too.
 */
static bool is_possible_partial(const char *s1, const char *s2) {
    size_t len1 = strlen(s1), len2 = strlen(s2);

    // Is one a prefix of the other?
    if (strncmp(s1, s2, len1 < len2 ? len1 : len2) == 0) {
        return true;
    }

    // Is one a suffix of the other?
    if (len1 < len2) {
        return strcmp(s1, s2 + len2 - len1) == 0;
    } else {
        return strcmp(s2, s1 + len1 - len2) == 0;
    }
}

/**
 * Retrieve the converted text put into our clip atom. In order for this to
 * happen a conversion must have been performed in an earlier iteration with
 * XConvertSelection.
 */
static char *get_clipboard_text(Atom clip_atom) {
    unsigned char *cur_text;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;

    int res =
        XGetWindowProperty(dpy, DefaultRootWindow(dpy), clip_atom, 0L, (~0L),
                           False, AnyPropertyType, &actual_type, &actual_format,
                           &nitems, &bytes_after, &cur_text);
    return res == Success ? (char *)cur_text : NULL;
}

/**
 * Return true if the given string contains any non-whitespace characters.
 */
static bool is_salient_text(const char *str) {
    if (!str) {
        return false;
    }

    for (; *str; str++) {
        if (!isspace((unsigned char)*str)) {
            return true;
        }
    }
    return false;
}

/**
 * Write the current enabled status to a designated status file.
 */
static void write_status(void) {
    _drop_(close) int fd =
        open(get_enabled_path(&cfg), O_WRONLY | O_CREAT, 0600);
    die_on(fd < 0, "Failed to update status: %s\n", strerror(errno));
    dprintf(fd, "%d", (int)enabled);
}

/**
 * Return true if the given window title matches the title of the clipserve
 * window.
 */
static bool is_clipserve(const char *win_title) {
    return win_title && streq(win_title, "clipserve");
}

/**
 * Determine if a window with the given title should be ignored based on user
 * configuration.
 */
static bool is_ignored_window(char *win_title) {
    if (!win_title || !cfg.ignore_window.set) {
        return 0;
    }
    int ret = regexec(&cfg.ignore_window.rgx, win_title, 0, NULL, 0);
    expect(ret == 0 || ret == REG_NOMATCH);
    return !ret;
}

/**
 * Disable or enable clip collection based on received signals.
 */
static void handle_signalfd_event(void) {
    struct signalfd_siginfo si;
    ssize_t s = read(sig_fd, &si, sizeof(struct signalfd_siginfo));
    expect(s == sizeof(struct signalfd_siginfo));
    dbg("Got signal %" PRIu32 " from pid %" PRIu32 "\n", si.ssi_signo,
        si.ssi_pid);
    switch (si.ssi_signo) {
        case SIGUSR1:
            enabled = 0;
            dbg("Clipboard collection disabled by signal\n");
            break;
        case SIGUSR2:
            enabled = 1;
            dbg("Clipboard collection enabled by signal\n");
            break;
    }
    write_status();
}

/**
 * Something changed about the watched selection, consider converting it to our
 * desired property type.
 */
static void handle_xfixes_selection_notify(XFixesSelectionNotifyEvent *se) {
    _drop_(XFree) char *win_title = get_window_title(dpy, se->owner);
    if (is_clipserve(win_title) || is_ignored_window(win_title)) {
        dbg("Ignoring clip from window titled '%s'\n", win_title);
        return;
    }

    enum selection_type sel =
        selection_atom_to_selection_type(se->selection, sels);
    dbg("Notified about selection update. Selection: %s, Owner: '%s' (0x%lx)\n",
        cfg.selections[sel].name, strnull(win_title), (unsigned long)se->owner);
    XConvertSelection(dpy, se->selection,
                      XInternAtom(dpy, "UTF8_STRING", False), sels[sel].storage,
                      win, CurrentTime);

    return;
}

/**
 * Something changed about the watched selection, but we don't explicitly
 * listen for SelectionNotify, so in reality this only happens in response to
 * an explicit request to tell us that there is no owner. In that case, return
 * -ENOENT.
 */
static int handle_selection_notify(const XSelectionEvent *se) {
    if (se->property == None) {
        enum selection_type sel =
            selection_atom_to_selection_type(se->selection, sels);
        dbg("X reports that %s has no current owner\n",
            cfg.selections[sel].name);
        return -ENOENT;
    }
    return 0;
}

/**
 * Trims the clip store if the number of clips exceeds the configured batch
 * size.
 */
static void maybe_trim(void) {
    uint64_t cur_clips;
    expect(cs_len(&cs, &cur_clips) == 0);
    if ((int)cur_clips > cfg.max_clips_batch) {
        expect(cs_trim(&cs, CS_ITER_NEWEST_FIRST, (size_t)cfg.max_clips) == 0);
    }
}

/**
 * Clips more than this many seconds apart are not considered for partial merge
 */
#define PARTIAL_MAX_SECS 2

/**
 * Store the clipboard text. If the text is a possible partial of the last clip
 * and it was received shortly afterwards, replace instead of adding.
 */
static uint64_t store_clip(char *text) {
    static char *last_text = NULL;
    static time_t last_text_time;

    dbg("Clipboard text is considered salient, storing\n");
    time_t current_time = time(NULL);
    uint64_t hash;
    if (last_text &&
        difftime(current_time, last_text_time) <= PARTIAL_MAX_SECS &&
        is_possible_partial(last_text, text)) {
        dbg("Possible partial of last clip, replacing\n");
        expect(cs_replace(&cs, CS_ITER_NEWEST_FIRST, 0, text, &hash) == 0);
    } else {
        expect(cs_add(&cs, text, &hash) == 0);
    }

    if (last_text) {
        XFree(last_text);
    }
    last_text = text;
    last_text_time = current_time;

    return hash;
}

/**
 * Something changed in our clip storage atoms. Work out whether we want to
 * store the new content as a clipboard entry.
 */
static int handle_property_notify(const XPropertyEvent *pe) {
    bool found = false;
    for (size_t i = 0; i < CM_SEL_MAX; ++i) {
        if (sels[i].storage == pe->atom) {
            found = true;
            break;
        }
    }
    if (!found || pe->state != PropertyNewValue) {
        return -EINVAL;
    }

    dbg("Received notification that selection conversion is ready\n");
    char *text = get_clipboard_text(pe->atom);
    char line[CS_SNIP_LINE_SIZE];
    first_line(text, line);
    dbg("First line: %s\n", line);

    if (is_salient_text(text)) {
        uint64_t hash = store_clip(text);
        maybe_trim();
        /* We only own CLIPBOARD because otherwise the behaviour is wonky:
         *
         *  1. When you select in a browser and press ^V, it repastes what you
         *     have selected instead of the previous content
         *  2. urxvt and some other terminal emulators will unhilight on PRIMARY
         *     ownership being taken away from them
         */
        enum selection_type sel =
            storage_atom_to_selection_type(pe->atom, sels);
        if (cfg.owned_selections[sel].active && cfg.own_clipboard) {
            run_clipserve(hash);
        }
    } else {
        dbg("Clipboard text is whitespace only, ignoring\n");
        XFree(text);
    }

    return 0;
}

/**
 * Process X11 events, returning when we have either processed one clip, or
 * have received an indication that the selection is not owned.
 *
 * The usual sequence is:
 *
 * 1. Get an XFixesSelectionNotify that we have a new selection.
 * 2. Call XConvertSelection() on it to get a string in our prop.
 * 3. Wait for a PropertyNotify that says that's ready.
 * 4. When it's ready, store it, and return from the function.
 *
 * Another possible outcome, especially when trying to get the initial state at
 * startup, is that we get a SelectionNotify even with owner == None, which
 * means the selection is unowned. At that point we also return, since it's
 * clear that an explicit request has been nacked.
 */
static int handle_x11_event(int evt_base) {
    while (XPending(dpy)) {
        XEvent evt;
        XNextEvent(dpy, &evt);

        if (!enabled) {
            dbg("Got X event, but ignoring as collection is disabled\n");
            continue;
        }

        int ret;
        if (evt.type == evt_base + XFixesSelectionNotify) {
            handle_xfixes_selection_notify((XFixesSelectionNotifyEvent *)&evt);
        } else if (evt.type == PropertyNotify) {
            ret = handle_property_notify((XPropertyEvent *)&evt);
            if (ret == 0) {
                return ret;
            }
        } else if (evt.type == SelectionNotify) {
            ret = handle_selection_notify((XSelectionEvent *)&evt);
            if (ret < 0) {
                return ret;
            }
        }
    }

    return -EINPROGRESS;
}

/**
 * Continuously wait for and process X11 or signal events until we fully
 * process success or failure for a clip.
 */
static int get_one_clip(int evt_base) {
    while (1) {
        // It's possible that we have more X events to process, but because of
        // the way the protocol works, we won't get told about them until we
        // next get an event if we wait for select(). Check for them first.
        if (XPending(dpy)) {
            return handle_x11_event(evt_base);
        }

        fd_set fds;
        int x_fd = ConnectionNumber(dpy);

        FD_ZERO(&fds);
        FD_SET(sig_fd, &fds);
        FD_SET(x_fd, &fds);

        int max_fd = sig_fd > x_fd ? sig_fd : x_fd;
        expect(select(max_fd + 1, &fds, NULL, NULL, NULL) > 0);

        if (FD_ISSET(sig_fd, &fds)) {
            handle_signalfd_event();
        }

        if (FD_ISSET(x_fd, &fds)) {
            return handle_x11_event(evt_base);
        }
    }
}

static int setup_watches(int evt_base) {
    XSelectInput(dpy, win, PropertyChangeMask);

    for (size_t i = 0; i < CM_SEL_MAX; i++) {
        struct selection sel = cfg.selections[i];
        if (!sel.active) {
            continue;
        }
        Atom sel_atom = sels[i].selection;
        XFixesSelectSelectionInput(dpy, win, sel_atom,
                                   XFixesSetSelectionOwnerNotifyMask);
        dbg("Getting initial value for selection %s\n", sel.name);
        XConvertSelection(dpy, sel_atom, XInternAtom(dpy, "UTF8_STRING", False),
                          sels[i].storage, win, CurrentTime);
        get_one_clip(evt_base);
    }

    return 0;
}

static int _noreturn_ run(int evt_base) {
    while (1) {
        get_one_clip(evt_base);
    }
}

#ifndef UNIT_TEST
int main(int argc, char *argv[]) {
    (void)argv;
    die_on(argc != 1, "clipmenud doesn't accept any arguments\n");
    int evt_base;

    cfg = setup("clipmenud");
    write_status();

    _drop_(close) int content_dir_fd = open(get_cache_dir(&cfg), O_RDONLY);
    _drop_(close) int snip_fd =
        open(get_line_cache_path(&cfg), O_RDWR | O_CREAT, 0600);
    expect(content_dir_fd >= 0 && snip_fd >= 0);

    expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);

    die_on(!(dpy = XOpenDisplay(NULL)), "Cannot open display\n");
    win = DefaultRootWindow(dpy);
    setup_selections(dpy, sels);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sig_fd = signalfd(-1, &mask, 0);
    expect(sig_fd >= 0);
    expect(signal(SIGCHLD, SIG_IGN) != SIG_ERR);

    int unused;
    die_on(!XFixesQueryExtension(dpy, &evt_base, &unused), "XFixes missing\n");

    setup_watches(evt_base);

    if (!cfg.oneshot) {
        run(evt_base);
    }

    expect(cs_destroy(&cs) == 0);
    config_free(&cfg);
    XCloseDisplay(dpy);
    return 0;
}
#endif
