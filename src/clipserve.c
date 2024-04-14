#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "store.h"
#include "util.h"
#include "x.h"

static Display *dpy;

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

    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    XStoreName(dpy, win, "clipserve");
    targets = XInternAtom(dpy, "TARGETS", False);
    utf8_string = XInternAtom(dpy, "UTF8_STRING", False);

    selections[1] = XInternAtom(dpy, "CLIPBOARD", False);
    for (size_t i = 0; i < arrlen(selections); i++) {
        XSetSelectionOwner(dpy, selections[i], win, CurrentTime);
        expect(XGetSelectionOwner(dpy, selections[i]) == win); // ICCCM 2.1
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
                dbg("Servicing request to window '%s' (0x%lx) for clip %" PRIu64
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
                    XChangeProperty(dpy, req->requestor, req->property,
                                    req->target, 8, PropModeReplace,
                                    (unsigned char *)content->data,
                                    (int)content->size);
                } else {
                    sev.property = None;
                }

                XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&sev);
                break;
            }
            case SelectionClear: {
                if (--remaining_selections == 0) {
                    dbg("Finished serving clip %" PRIu64 "\n", hash);
                    running = false;
                } else {
                    dbg("%d selections remaining to serve for clip %" PRIu64
                        "\n",
                        remaining_selections, hash);
                }
                break;
            }
        }
    }

    XCloseDisplay(dpy);
}

int main(int argc, char *argv[]) {
    die_on(argc != 2, "Usage: clipserve [hash]\n");
    _drop_(config_free) struct config cfg = setup("clipserve");

    uint64_t hash;
    expect(str_to_uint64(argv[1], &hash) == 0);

    _drop_(close) int content_dir_fd = open(get_cache_dir(&cfg), O_RDONLY);
    _drop_(close) int snip_fd =
        open(get_line_cache_path(&cfg), O_RDWR | O_CREAT, 0600);
    expect(content_dir_fd >= 0 && snip_fd >= 0);

    _drop_(cs_destroy) struct clip_store cs;
    expect(cs_init(&cs, snip_fd, content_dir_fd) == 0);

    _drop_(cs_content_unmap) struct cs_content content;
    die_on(cs_content_get(&cs, hash, &content) < 0,
           "Hash %" PRIu64 " inaccessible\n", hash);

    serve_clipboard(hash, &content);

    return 0;
}
