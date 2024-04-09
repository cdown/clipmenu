#ifndef CM_X_H
#define CM_X_H

#include <X11/Xlib.h>

#include "util.h"

DEFINE_DROP_FUNC_VOID(XFree)

char _nonnull_ *get_window_title(Display *dpy, Window owner);
int xerror_handler(Display *dpy _unused_, XErrorEvent *ee);

#endif
