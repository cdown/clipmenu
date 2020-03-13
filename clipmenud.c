/* gcc clipmenud.c -Wall -Werror -lxcb -o cd */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>

#define die(status, ...)                                                       \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(status);

/*
 * Create a hidden child of the root window to collect clipboard events, and
 * then return its XID.
 */
static int create_event_window(xcb_connection_t *xcb_conn, xcb_window_t *xid) {
    xcb_screen_t *screen;
    xcb_generic_error_t *err;
    xcb_void_cookie_t cookie;
    xcb_window_t window_xid = xcb_generate_id(xcb_conn);
    uint32_t values[3] = {0, 1, XCB_EVENT_MASK_PROPERTY_CHANGE};

    screen = xcb_setup_roots_iterator(xcb_get_setup(xcb_conn)).data;
    values[0] = screen->black_pixel;

    /* Window dimensions must be >0, but we'll deal with that later. */
    cookie = xcb_create_window_checked(
        xcb_conn, screen->root_depth, window_xid, screen->root, 0, 0, 1, 1, 0,
        XCB_COPY_FROM_PARENT, screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
        values);

    err = xcb_request_check(xcb_conn, cookie);
    if (err) {
        free(err);
        return -EIO;
    }

    xcb_map_window(xcb_conn, window_xid);
    xcb_flush(xcb_conn);
    *xid = window_xid;

    return 0;
}

/* Destroy the event capture window. */
static int destroy_event_window(xcb_connection_t *xcb_conn, xcb_window_t xid) {
    xcb_generic_error_t *err;
    xcb_void_cookie_t cookie = xcb_destroy_window_checked(xcb_conn, xid);

    err = xcb_request_check(xcb_conn, cookie);
    if (err) {
        free(err);
        return -EIO;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    xcb_connection_t *xcb_conn;
    xcb_window_t event_win;
    int err;

    xcb_conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(xcb_conn)) {
        xcb_disconnect(xcb_conn);
        die(1, "Failed to connect to X server\n");
    }

    err = create_event_window(xcb_conn, &event_win);
    if (err) {
        xcb_disconnect(xcb_conn);
        die(2, "Failed to create event window\n");
    }

    err = destroy_event_window(xcb_conn, event_win);
    if (err) {
        xcb_disconnect(xcb_conn);
        die(3, "Failed to destroy event window\n");
    }

    xcb_disconnect(xcb_conn);
}
