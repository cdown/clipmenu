#ifndef CM_X_H
#define CM_X_H

#include <X11/Xlib.h>

#include "util.h"

DEFINE_DROP_FUNC_VOID(XFree)

size_t _nonnull_ get_chunk_size(Display *dpy);
char _nonnull_ *get_window_title(Display *dpy, Window owner);
int xerror_handler(Display *dpy _unused_, XErrorEvent *ee);

struct incr_transfer {
    struct incr_transfer *next;
    struct incr_transfer *prev;
    Window requestor;
    Atom property;
    Atom target;
    int format;
    char *data;
    size_t data_size;
    size_t data_capacity;
    size_t offset;
};

#define it_dbg(it, fmt, ...)                                                   \
    dbg("[incr 0x%lx] " fmt, (unsigned long)(it)->requestor, ##__VA_ARGS__)
void _nonnull_ it_add(struct incr_transfer **it_list, struct incr_transfer *it);
void _nonnull_ it_remove(struct incr_transfer **it_list,
                         struct incr_transfer *it);

#endif
