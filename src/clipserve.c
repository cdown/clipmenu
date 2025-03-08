#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "store.h"
#include "util.h"
#include "x.h"

static struct incr_transfer *it_list = NULL;
static Display *dpy;
static Atom incr_atom;

static size_t chunk_size;

/**
 * Start an INCR transfer.
 */
static void incr_send_start(XSelectionRequestEvent *req,
                            struct cs_content *content) {
    long incr_size = content->size;
    XChangeProperty(dpy, req->requestor, req->property, incr_atom, 32,
                    PropModeReplace, (unsigned char *)&incr_size, 1);

    struct incr_transfer *it = malloc(sizeof(struct incr_transfer));
    expect(it);
    *it = (struct incr_transfer){
        .requestor = req->requestor,
        .property = req->property,
        .target = req->target,
        .format = 8,
        .data = (char *)content->data,
        .data_size = content->size,
        .offset = 0,
    };

    it_dbg(it, "Starting transfer\n");
    it_add(&it_list, it);

    // Listen for PropertyNotify events on the requestor window
    XSelectInput(dpy, it->requestor, PropertyChangeMask);
}

/**
 * Finish an INCR transfer.
 */
static void incr_send_finish(struct incr_transfer *it) {
    XChangeProperty(dpy, it->requestor, it->property, it->target, it->format,
                    PropModeReplace, NULL, 0);
    it_dbg(it, "Transfer complete\n");
    it_remove(&it_list, it);
    free(it);
}

/**
 * Continue sending data during an INCR transfer.
 */
static void incr_send_chunk(const XPropertyEvent *pe) {
    if (pe->state != PropertyDelete) {
        return;
    }

    struct incr_transfer *it = it_list;
    while (it) {
        if (it->requestor == pe->window && it->property == pe->atom) {
            size_t remaining = it->data_size - it->offset;
            size_t this_chunk_size =
                (remaining > chunk_size) ? chunk_size : remaining;

            it_dbg(it,
                   "Sending chunk (bytes sent: %zu, bytes remaining: %zu)\n",
                   it->offset, remaining);

            if (this_chunk_size > 0) {
                XChangeProperty(dpy, it->requestor, it->property, it->target,
                                it->format, PropModeReplace,
                                (unsigned char *)(it->data + it->offset),
                                this_chunk_size);
                it->offset += this_chunk_size;
            } else {
                incr_send_finish(it);
            }
            break;
        }
        it = it->next;
    }
}

/**
 * Serve clipboard content for all X11 selection requests until all selections
 * have been claimed by another application.
 */
static void _nonnull_ serve_clipboard(uint64_t hash,
                                      struct cs_content *content) {
    bool running = true;
    XEvent evt;
    Atom targets, utf8_string, selections[2] = {XA_PRIMARY};
    Window win;
    int remaining_selections;

    dpy = XOpenDisplay(NULL);
    expect(dpy);

    chunk_size = get_chunk_size(dpy);

    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    XStoreName(dpy, win, "clipserve");
    targets = XInternAtom(dpy, "TARGETS", False);
    utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    incr_atom = XInternAtom(dpy, "INCR", False);

    selections[1] = XInternAtom(dpy, "CLIPBOARD", False);
    for (size_t i = 0; i < arrlen(selections); i++) {
        bool success = false;
        for (int attempts = 0; attempts < 5; attempts++) {
            XSetSelectionOwner(dpy, selections[i], win, CurrentTime);

            // According to ICCCM 2.1, a client acquiring a selection should
            // confirm success by verifying with GetSelectionOwner.
            if (XGetSelectionOwner(dpy, selections[i]) == win) {
                success = true;
                break;
            }
        }
        if (!success) {
            die("Failed to set selection for %s\n",
                XGetAtomName(dpy, selections[i]));
        }
    }
    remaining_selections = arrlen(selections);

    while (running) {
        XNextEvent(dpy, &evt);
        switch (evt.type) {
            case SelectionRequest: {
                XSelectionRequestEvent *req = &evt.xselectionrequest;
                XSelectionEvent sev = {.type = SelectionNotify,
                                       .display = req->display,
                                       .requestor = req->requestor,
                                       .selection = req->selection,
                                       .time = req->time,
                                       .target = req->target,
                                       .property = req->property};

                _drop_(XFree) char *window_title =
                    get_window_title(dpy, req->requestor);
                dbg("Servicing request to window '%s' (0x%lX) for clip " PRI_HASH
                    "\n",
                    strnull(window_title), (unsigned long)req->requestor, hash);

                if (req->target == targets) {
                    Atom available_targets[] = {utf8_string, XA_STRING};
                    XChangeProperty(dpy, req->requestor, req->property, XA_ATOM,
                                    32, PropModeReplace,
                                    (unsigned char *)&available_targets,
                                    arrlen(available_targets));
                } else if (req->target == utf8_string ||
                           req->target == XA_STRING) {
                    if (content->size < (off_t)chunk_size) {
                        // Data size is small enough, send directly
                        XChangeProperty(dpy, req->requestor, req->property,
                                        req->target, 8, PropModeReplace,
                                        (unsigned char *)content->data,
                                        (int)content->size);
                    } else {
                        // Initiate INCR transfer
                        incr_send_start(req, content);
                    }
                } else {
                    sev.property = None;
                }

                XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&sev);
                break;
            }
            case SelectionClear: {
                if (--remaining_selections == 0) {
                    dbg("Finished serving clip " PRI_HASH "\n", hash);
                    running = false;
                } else {
                    dbg("%d selections remaining to serve for clip " PRI_HASH
                        "\n",
                        remaining_selections, hash);
                }
                break;
            }
            case PropertyNotify: {
                incr_send_chunk(&evt.xproperty);
                break;
            }
        }
    }

    XCloseDisplay(dpy);
}

int main(int argc, char *argv[]) {
    die_on(argc != 2, "Usage: clipserve [hash]\n");
    _drop_(config_free) struct config cfg = setup("clipserve");
    exec_man_on_help(argc, argv);

    uint64_t hash;
    expect(str_to_hex64(argv[1], &hash) == 0);

    _drop_(close) int content_dir_fd = open(get_cache_dir(&cfg), O_RDONLY);
    _drop_(close) int snip_fd =
        open(get_line_cache_path(&cfg), O_RDWR | O_CREAT, 0600);
    expect(content_dir_fd >= 0 && snip_fd >= 0);

    _drop_(cs_destroy) struct clip_store cs;
    expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);

    _drop_(cs_content_unmap) struct cs_content content;
    die_on(cs_content_get(&cs, hash, &content) < 0,
           "Hash " PRI_HASH " inaccessible\n", hash);

    serve_clipboard(hash, &content);

    return 0;
}
