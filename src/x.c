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
        (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
        return 0;
    die("X error with request code=%d, error code=%d\n", ee->request_code,
        ee->error_code);
}
