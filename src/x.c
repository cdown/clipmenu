#include <X11/Xatom.h>
#include <X11/Xproto.h>
#include <stddef.h>

#include "x.h"

/**
 * Fetch the title of the window with the specified window ID.
 */
char *get_window_title(Display *dpy, Window owner) {
    Atom props[] = {XInternAtom(dpy, "_NET_WM_NAME", False), XA_WM_NAME};
    Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    Atom actual_type;
    int format;
    unsigned long nr_items, bytes_after;
    unsigned char *prop = NULL;

    for (size_t i = 0; i < arrlen(props); i++) {
        if (XGetWindowProperty(dpy, owner, props[i], 0, (~0L), False,
                               (props[i] == XA_WM_NAME) ? AnyPropertyType
                                                        : utf8_string,
                               &actual_type, &format, &nr_items, &bytes_after,
                               &prop) == Success &&
            prop) {
            return (char *)prop;
        }
    }
    return NULL;
}

/**
 * Certain X11 operations may fail in expected ways. For example, when
 * attempting to interact with a window that has been closed. This handler
 * avoids the application terminating in such cases.
 *
 * The cppcheck suppression is for a false positive: this is a callback and
 * cannot be changed.
 */
// cppcheck-suppress [constParameterPointer,unmatchedSuppression]
int xerror_handler(Display *dpy _unused_, XErrorEvent *ee) {
    if (ee->error_code == BadWindow ||
        (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch) ||
        (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable) ||
        (ee->request_code == X_PolyFillRectangle &&
         ee->error_code == BadDrawable) ||
        (ee->request_code == X_PolySegment && ee->error_code == BadDrawable) ||
        (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch) ||
        (ee->request_code == X_GrabButton && ee->error_code == BadAccess) ||
        (ee->request_code == X_GrabKey && ee->error_code == BadAccess) ||
        (ee->request_code == X_CopyArea && ee->error_code == BadDrawable)) {
        return 0;
    }
    die("X error with request code=%d, error code=%d\n", ee->request_code,
        ee->error_code);
}

#define FALLBACK_CHUNK_BYTES 4 * 1024

/**
 * Return a quarter of the maximum request size in bytes.
 *
 * XMaxRequestSize and XExtendedMaxRequestSize return units of 4-byte words, so
 * the word count already equals 1/4 of the maximum request size in bytes.
 */
size_t get_incr_threshold(Display *dpy) {
    size_t max_request_words = XExtendedMaxRequestSize(dpy);
    if (max_request_words == 0) {
        max_request_words = XMaxRequestSize(dpy);
    }
    return max_request_words ? max_request_words : FALLBACK_CHUNK_BYTES;
}

/**
 * Calculate an appropriate INCR payload size.
 *
 * The request size word count is a sensible threshold for deciding when to use
 * INCR at all, but using that same value as the payload of each
 * XChangeProperty request is too aggressive in practice.
 *
 * This is an interoperability constraint, not a protocol-limit one, because:
 *
 * - XMaxRequestSize/XExtendedMaxRequestSize only tell us what request size the
 *   X server will accept.
 * - INCR has no chunk-size negotiation, so if a requestor dislikes large
 *   payloads it can simply stop deleting the property, leaving the sender
 *   waiting forever.
 *
 * Other implementations do not agree on a single rule here either: xclip uses
 * a heuristic, some clipboard managers use a fixed cap, and Emacs couples
 * large chunks with a timeout.
 */
size_t get_chunk_size(Display *dpy) {
    size_t incr_threshold = get_incr_threshold(dpy);
    return incr_threshold / 4 ? incr_threshold / 4 : FALLBACK_CHUNK_BYTES;
}

/**
 * Add a new INCR transfer to the active list.
 */
void it_add(struct incr_transfer **it_list, struct incr_transfer *it) {
    if (*it_list) {
        (*it_list)->prev = it;
    }
    it->next = *it_list;
    it->prev = NULL;
    *it_list = it;
}
/**
 * Remove an INCR transfer from the active list.
 */
void it_remove(struct incr_transfer **it_list, struct incr_transfer *it) {
    if (it->prev) {
        it->prev->next = it->next;
    }
    if (it->next) {
        it->next->prev = it->prev;
    }
    if (*it_list == it) {
        *it_list = it->next;
    }
}
