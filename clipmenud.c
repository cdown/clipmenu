/* gcc clipmenud.c -Wall -Werror -lxcb -lxcb-util -lxcb-xfixes -o cd */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xfixes.h>

#define die(status, ...)                                                       \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(status);

static xcb_connection_t *xcb_conn;
static xcb_window_t evt_win;

static int print_xcb_error(xcb_generic_error_t *err) {
    if (!err) {
        return 0;
    }

    fprintf(stderr, "X error %d: %s\n", err->error_code,
            xcb_event_get_error_label(err->error_code));

    return -1;
}

static int get_atom(const char *name, xcb_atom_t *atom) {
    xcb_intern_atom_cookie_t cookie;
    xcb_intern_atom_reply_t *ret = NULL;

    cookie = xcb_intern_atom(xcb_conn, 0, strlen(name), name);
    ret = xcb_intern_atom_reply(xcb_conn, cookie, NULL);
    if (!ret) {
        return -EINVAL;
    }

    *atom = ret->atom;
    free(ret);

    return 0;
}

/*
 * Create a hidden child of the root window to collect clipboard events, and
 * then return its XID.
 */
static int create_event_window(xcb_window_t *xid) {
    xcb_screen_t *screen;
    xcb_generic_error_t *err;
    xcb_void_cookie_t cookie;
    xcb_window_t window_xid = xcb_generate_id(xcb_conn);
    uint32_t values[3] = {0, 1, XCB_EVENT_MASK_PROPERTY_CHANGE};

    screen = xcb_setup_roots_iterator(xcb_get_setup(xcb_conn)).data;
    values[0] = screen->black_pixel;

    /*
     * Window dimensions are irrelevant since it won't be displayed, but X
     * returns BadRequest for <0.
     */
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
static int destroy_event_window(void) {
    xcb_generic_error_t *err;
    xcb_void_cookie_t cookie = xcb_destroy_window_checked(xcb_conn, evt_win);

    err = xcb_request_check(xcb_conn, cookie);
    if (err) {
        free(err);
        return -EIO;
    }
    return 0;
}

static int set_notify(const char *sel_name) {
    xcb_atom_t atom;
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *err;
    int ret;

    ret = get_atom(sel_name, &atom);
    if (ret) {
        return ret;
    }

    xcb_discard_reply(
        xcb_conn, xcb_xfixes_query_version(xcb_conn, XCB_XFIXES_MAJOR_VERSION,
                                           XCB_XFIXES_MINOR_VERSION)
                      .sequence);

    cookie = xcb_xfixes_select_selection_input_checked(
        xcb_conn, evt_win, atom,
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER);

    err = xcb_request_check(xcb_conn, cookie);
    if (err) {
        print_xcb_error(err);
        free(err);
        return -EIO;
    }

    return 0;
}

static int get_xfixes_first_event(void) {
    const xcb_query_extension_reply_t *reply;

    reply = xcb_get_extension_data(xcb_conn, &xcb_xfixes_id);

    if (!reply || !reply->present) {
        return -ENOSYS;
    }

    /* uint8_t -> signed int */
    return reply->first_event;
}

static void event_loop() {
    xcb_generic_event_t *evt;
    int ret;
    int xfixes_loc = get_xfixes_first_event();

    if (xfixes_loc < 0) {
        die(89, "XFixes extension unavailable\n");
    }

    ret = set_notify("PRIMARY");
    if (ret) {
        die(90, "Error setting up notifications for PRIMARY\n");
    }

    ret = set_notify("CLIPBOARD");
    if (ret) {
        die(91, "Error setting up notifications for CLIPBOARD\n");
    }

    xcb_flush(xcb_conn);

    while ((evt = xcb_wait_for_event(xcb_conn))) {
        uint8_t resp_type;

        if (!evt) {
            fprintf(stderr, "I/O error getting event from X server\n");
            continue;
        }

        if (evt->response_type == 0) {
            fprintf(stderr, "Unknown error getting event from X server\n");
            continue;
        }

        switch ((resp_type = XCB_EVENT_RESPONSE_TYPE(evt))) {
        case XCB_SELECTION_NOTIFY:
            printf("Got XCB_SELECTION_NOTIFY for %d\n",
                   ((xcb_selection_notify_event_t *)evt)->selection);
            break;

        default:
            /*
             * Not integer constant, so has to be evaluated outside of the
             * switch
             */
            if (resp_type == xfixes_loc + XCB_XFIXES_SELECTION_NOTIFY) {
                printf("Got XCB_XFIXES_SELECTION_NOTIFY for %d\n",
                       ((xcb_xfixes_selection_notify_event_t *)evt)->selection);
            }
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    int err;

    xcb_conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(xcb_conn)) {
        xcb_disconnect(xcb_conn);
        die(1, "Failed to connect to X server\n");
    }

    err = create_event_window(&evt_win);
    if (err) {
        xcb_disconnect(xcb_conn);
        die(2, "Failed to create event window\n");
    }

    event_loop();

    err = destroy_event_window();
    if (err) {
        xcb_disconnect(xcb_conn);
        die(3, "Failed to destroy event window\n");
    }

    xcb_disconnect(xcb_conn);
}
