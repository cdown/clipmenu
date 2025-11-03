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
#include <sys/file.h>
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

static Atom timestamp_atom;
static Atom incr_atom;
static struct incr_transfer *it_list;

static struct cm_selections sels[CM_SEL_MAX];

static Time last_disable_time = 0;
static Time last_enable_time = 0;

enum clip_text_source {
    CLIP_TEXT_SOURCE_X,
    CLIP_TEXT_SOURCE_MALLOC,
    CLIP_TEXT_SOURCE_INVALID
};

struct clip_text {
    char *data;
    enum clip_text_source source;
};

static void free_clip_text(struct clip_text *ct) {
    expect(ct->source != CLIP_TEXT_SOURCE_INVALID);

    if (ct->data) {
        if (ct->source == CLIP_TEXT_SOURCE_X) {
            XFree(ct->data);
        } else {
            free(ct->data);
        }
        ct->data = NULL;
    }

    ct->source = CLIP_TEXT_SOURCE_INVALID;
}

// cppcheck-suppress [constParameterCallback,unmatchedSuppression]
static Bool is_timestamp_event(Display *display _unused_, XEvent *event,
                               XPointer arg) {
    return event->type == PropertyNotify &&
           event->xproperty.atom == *(Atom *)arg;
}

/**
 * Get the current X server time by triggering a PropertyNotify.
 */
static Time get_current_server_time(void) {
    XEvent ev;
    XChangeProperty(dpy, win, timestamp_atom, XA_INTEGER, 32, PropModeReplace,
                    NULL, 0);
    XIfEvent(dpy, &ev, is_timestamp_event, (XPointer)&timestamp_atom);
    return ev.xproperty.time;
}

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
static struct clip_text get_clipboard_text(Atom clip_atom) {
    struct clip_text ct = {NULL, CLIP_TEXT_SOURCE_X};
    unsigned char *cur_text;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;

    int res = XGetWindowProperty(dpy, win, clip_atom, 0L, (~0L), False,
                                 AnyPropertyType, &actual_type, &actual_format,
                                 &nitems, &bytes_after, &cur_text);
    if (res != Success) {
        return ct;
    }

    if (actual_type == incr_atom) {
        dbg("Unexpected INCR transfer detected\n");
        XFree(cur_text);
        return ct;
    }

    ct.data = (char *)cur_text;

    return ct;
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
            // If we're already disabled, we need to keep the original
            // timestamp to properly filter all events that were queued during
            // any part of the disabled period.
            if (enabled) {
                last_disable_time = get_current_server_time();
            }
            enabled = 0;
            dbg("Clipboard collection disabled by signal at time %lu\n",
                (unsigned long)last_disable_time);
            break;
        case SIGUSR2:
            // If we're already enabled, we need to keep the original timestamp
            // so we don't mistakenly filter out valid messages.
            if (!enabled) {
                last_enable_time = get_current_server_time();
            }
            enabled = 1;
            dbg("Clipboard collection enabled by signal at time %lu\n",
                (unsigned long)last_enable_time);
            break;
    }
    write_status();
}

/**
 * Something changed about the watched selection, consider converting it to our
 * desired property type.
 */
static void handle_xfixes_selection_notify(XFixesSelectionNotifyEvent *se) {
    if (last_disable_time > 0 && se->timestamp >= last_disable_time &&
        se->timestamp < last_enable_time) {
        dbg("Ignoring selection event from disabled period (event time: %lu, disabled: %lu, enabled: %lu)\n",
            (unsigned long)se->timestamp, (unsigned long)last_disable_time,
            (unsigned long)last_enable_time);
        return;
    }

    enum selection_type sel =
        selection_atom_to_selection_type(se->selection, sels);
    if (sel == CM_SEL_INVALID) {
        dbg("Received XFixesSelectionNotify for unknown sel\n");
        return;
    }

    _drop_(XFree) char *win_title = get_window_title(dpy, se->owner);
    if (is_clipserve(win_title) || is_ignored_window(win_title)) {
        dbg("Ignoring clip from window titled '%s'\n", win_title);
        return;
    }

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
        if (sel == CM_SEL_INVALID) {
            dbg("Received no owner notification for unknown sel\n");
            return 0;
        }
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
    size_t cur_clips;
    expect(cs_len(&cs, &cur_clips) == 0);
    if (cur_clips > (size_t)cfg.max_clips + (size_t)cfg.max_clips_batch) {
        expect(cs_trim(&cs, CS_ITER_NEWEST_FIRST, (size_t)cfg.max_clips) == 0);
    }
}

/**
 * Store the clipboard text. If the text is a possible partial of the last clip
 * and it was received shortly afterwards, replace instead of adding.
 */
static uint64_t store_clip(struct clip_text *ct) {
    static struct clip_text last_text = {NULL, CLIP_TEXT_SOURCE_MALLOC};
    static time_t last_text_time;

    dbg("Clipboard text is considered salient, storing\n");
    time_t current_time = time(NULL);
    uint64_t hash;

    if (cfg.partial_merge_secs > 0 && last_text.data &&
        difftime(current_time, last_text_time) <= cfg.partial_merge_secs &&
        is_possible_partial(last_text.data, ct->data)) {
        dbg("Possible partial of last clip, replacing\n");
        expect(cs_replace(&cs, CS_ITER_NEWEST_FIRST, 0, ct->data, &hash) == 0);
    } else {
        expect(cs_add(&cs, ct->data, &hash,
                      cfg.deduplicate ? CS_DUPE_KEEP_LAST : CS_DUPE_KEEP_ALL) ==
               0);
    }

    free_clip_text(&last_text);
    last_text = *ct;
    last_text_time = current_time;

    // The caller no longer owns this data.
    ct->data = NULL;
    ct->source = CLIP_TEXT_SOURCE_INVALID;

    return hash;
}

/**
 * Process the final data collected during an INCR transfer.
 */
static void incr_receive_finish(struct incr_transfer *it) {
    enum selection_type sel =
        storage_atom_to_selection_type(it->property, sels);
    if (sel == CM_SEL_INVALID) {
        it_dbg(it, "Received INCR finish for unknown sel\n");
        return;
    }

    it_dbg(it, "Finished (bytes buffered: %zu)\n", it->data_size);
    char *text = malloc(it->data_size + 1);
    expect(text);
    memcpy(text, it->data, it->data_size);
    text[it->data_size] = '\0';

    struct clip_text ct = {text, CLIP_TEXT_SOURCE_MALLOC};

    char line[CS_SNIP_LINE_SIZE];
    first_line(ct.data, line);
    it_dbg(it, "First line: %s\n", line);

    if (is_salient_text(ct.data)) {
        uint64_t hash = store_clip(&ct);
        maybe_trim();
        if (cfg.owned_selections[sel].active && cfg.own_clipboard) {
            run_clipserve(hash);
        }
    } else {
        it_dbg(it, "Clipboard text is whitespace only, ignoring\n");
        free_clip_text(&ct);
    }

    free(it->data);
    it_remove(&it_list, it);
    free(it);
}

#define INCR_DATA_START_BYTES 1024 * 1024

/**
 * Acknowledge and start an INCR transfer.
 */
static void incr_receive_start(const XPropertyEvent *pe) {
    struct incr_transfer *it = malloc(sizeof(struct incr_transfer));
    expect(it);
    *it = (struct incr_transfer){
        .property = pe->atom,
        .requestor = pe->window,
        .data_size = 0,
        .data_capacity = INCR_DATA_START_BYTES,
        .data = malloc(INCR_DATA_START_BYTES),
    };
    expect(it->data);

    it_dbg(it, "Starting transfer\n");
    it_add(&it_list, it);

    // Signal readiness for chunks
    XDeleteProperty(dpy, win, pe->atom);
}

/**
 * Continue receiving data during an INCR transfer.
 */
static void incr_receive_data(const XPropertyEvent *pe,
                              struct incr_transfer *it) {
    if (pe->state != PropertyNewValue) {
        return;
    }

    it_dbg(it, "Receiving chunk (bytes buffered: %zu)\n", it->data_size);

    _drop_(XFree) unsigned char *chunk = NULL;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    XGetWindowProperty(dpy, win, pe->atom, 0, LONG_MAX, False, AnyPropertyType,
                       &actual_type, &actual_format, &nitems, &bytes_after,
                       &chunk);

    size_t chunk_size = nitems * (actual_format / 8);

    if (chunk_size == 0) {
        it_dbg(it, "Transfer complete\n");
        incr_receive_finish(it);
        return;
    }

    if (it->data_size + chunk_size > it->data_capacity) {
        it->data_capacity = (it->data_size + chunk_size) * 2;
        it->data = realloc(it->data, it->data_capacity);
        expect(it->data);
        it_dbg(it, "Expanded data buffer to %zu bytes\n", it->data_capacity);
    }

    memcpy(it->data + it->data_size, chunk, chunk_size);
    it->data_size += chunk_size;

    // Signal readiness for next chunk
    XDeleteProperty(dpy, win, pe->atom);
}

/**
 * Something changed in our clip storage atoms. Work out whether we want to
 * store the new content as a clipboard entry.
 */
static int handle_property_notify(const XPropertyEvent *pe) {
    if (pe->state != PropertyNewValue && pe->state != PropertyDelete) {
        return -EINVAL;
    }

    enum selection_type sel = storage_atom_to_selection_type(pe->atom, sels);
    if (sel == CM_SEL_INVALID) {
        dbg("Received PropertyNotify for unknown sel\n");
        return -EINVAL;
    }

    // Check if this property corresponds to an INCR transfer in progress
    struct incr_transfer *it = it_list;
    while (it) {
        if (it->property == pe->atom && it->requestor == pe->window) {
            break;
        }
        it = it->next;
    }

    if (it) {
        incr_receive_data(pe, it);
        return 0;
    }

    // Not an INCR transfer in progress. Check if this is an INCR transfer
    // starting
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    _drop_(XFree) unsigned char *prop = NULL;

    XGetWindowProperty(dpy, win, pe->atom, 0, 0, False, AnyPropertyType,
                       &actual_type, &actual_format, &nitems, &bytes_after,
                       &prop);

    if (actual_type == incr_atom) {
        incr_receive_start(pe);
    } else {
        dbg("Received non-INCR PropertyNotify\n");

        // store_clip will take care of freeing this later when it's gone from
        // last_text.
        struct clip_text ct = get_clipboard_text(pe->atom);
        if (!ct.data) {
            dbg("Failed to get clipboard text\n");
            return -EINVAL;
        }
        char line[CS_SNIP_LINE_SIZE];
        first_line(ct.data, line);
        dbg("First line: %s\n", line);

        if (is_salient_text(ct.data)) {
            uint64_t hash = store_clip(&ct);
            maybe_trim();
            /* We only own CLIPBOARD because otherwise the behaviour is wonky:
             *
             *  1. When you select in a browser and press ^V, it repastes what
             *     you have selected instead of the previous content
             *  2. urxvt and some other terminal emulators will unhilight on
             *     PRIMARY ownership being taken away from them
             */
            if (cfg.owned_selections[sel].active && cfg.own_clipboard) {
                run_clipserve(hash);
            }
        } else {
            dbg("Clipboard text is whitespace only, ignoring\n");
            free_clip_text(&ct);
        }
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
    exec_man_on_help(argc, argv);

    _drop_(close) int session_fd =
        open(get_session_lock_path(&cfg), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    die_on(session_fd < 0, "Failed to open session file: %s\n",
           strerror(errno));
    die_on(flock(session_fd, LOCK_EX | LOCK_NB) < 0,
           "Failed to lock session file -- is another clipmenud running?\n");

    write_status();

    _drop_(close) int content_dir_fd = open(get_cache_dir(&cfg), O_RDONLY);
    _drop_(close) int snip_fd =
        open(get_line_cache_path(&cfg), O_RDWR | O_CREAT, 0600);
    expect(content_dir_fd >= 0 && snip_fd >= 0);

    expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);

    die_on(!(dpy = XOpenDisplay(NULL)), "Cannot open display\n");
    win = DefaultRootWindow(dpy);
    setup_selections(dpy, sels);

    incr_atom = XInternAtom(dpy, "INCR", False);
    timestamp_atom = XInternAtom(dpy, "CLIPMENUD_TIMESTAMP", False);

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
