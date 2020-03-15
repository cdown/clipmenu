/* gcc -g3 -fsanitize=address -fno-omit-frame-pointer clipmenud.c -Wall -Werror
 * -lxcb -lxcb-util -lxcb-xfixes -o cd */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xfixes.h>

#define die(status, ...)                                                       \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(status);

#define min(x, y) (((x) < (y)) ? (x) : (y))

#define SIZE_T_STRING_MAX 20

static xcb_connection_t *xcb_conn;
static xcb_window_t evt_win;

/*
 * A totally insecure hash function with decent speed and distribution.
 *
 * https://groups.google.com/forum/#!msg/comp.lang.c/lSKWXiuNOAk/zstZ3SRhCjgJ
 */
static uint32_t djb2_hash(const char *key) {
    uint32_t i, hash = 5381;

    for (i = 0; i < strlen(key); i++)
        hash = ((hash << 5) + hash) + key[i];

    return hash;
}

static void *malloc_checked(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "ENOMEM: failed to allocate %zu bytes\n", n);
        abort();
    }
    return p;
}

/*
 * Get the first line. If there's only one, it will look something like this:
 *
 *     The first line
 *
 * If there are multiple, it will look like this:
 *
 *     The first line (3 lines)
 *
 * We ignore leading empty lines in order to try to display something actually
 * useful.
 *
 * The result of this function must be freed when unused.
 */
static char *get_first_line(char *data) {
    size_t i, num_lines;
    size_t alloc_size = 0;
    char *first_line_start = NULL, *first_line_end = NULL;
    char *output = NULL;
    int sn_i = 0;
    ptrdiff_t first_line_length;

    first_line_start = (char *)data;

    for (i = 0, num_lines = 0; data[i]; i++) {
        if (data[i] == '\n') {
            ++num_lines;
            /* If it's just an empty line, we don't want it */
            if (!first_line_end && i && data[i - 1] != '\n') {
                first_line_end = (char *)data + i;
            }
        } else if (!first_line_end && i && data[i - 1] == '\n') {
            /* Record a potential start */
            first_line_start = (char *)data + i;
        }
    }

    assert(!first_line_end || first_line_start < first_line_end);

    /*
     * If there was no newline at the end, still count it as a line, and set
     * first_line_end as appropriate if we used that line as the first line,
     * otherwise we'd miss it since it isn't a newline.
     */
    if (i && !first_line_end && data[i - 1] != '\n') {
        first_line_end = (char *)data + i;
    }

    if (i && data[i - 1] != '\n') {
        ++num_lines;
    }

    if (first_line_end) {
        first_line_length = first_line_end - first_line_start;
        alloc_size += first_line_length;
    }

    if (num_lines > 1) {
        alloc_size += strlen(" (") + SIZE_T_STRING_MAX + strlen(" lines)");
    }

    ++alloc_size; /* \0 */

    output = malloc_checked(alloc_size);

    /* In case the content is "" or "\n" and we don't put in anything */
    output[0] = '\0';

    if (first_line_end) {
        assert(first_line_length <= alloc_size);
        /* +1 to avoid truncating for trailing null */
        (void)snprintf(output, first_line_length + 1, "%s", first_line_start);
        sn_i += first_line_length;
    }

    if (num_lines > 1) {
        (void)snprintf(output + sn_i, alloc_size - sn_i, " (%zu lines)",
                       num_lines);
    }

    return output;
}

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

static void handle_xfixes_selection_notify(xcb_generic_event_t *raw_evt) {
#if 0
    int ret;
    xcb_atom_t utf8_string_atom, xsel_data_atom;
    xcb_xfixes_selection_notify_event_t *evt =
        (xcb_xfixes_selection_notify_event_t *)raw_evt;

    /*
     * If this happens, we're already in control and don't need to do anything,
     * likely from the xcb_set_selection_owner below.
     */
    if (evt->owner == evt_win) {
        return;
    }

    ret = get_atom("XSEL_DATA", &xsel_data_atom);
    if (ret) {
        fprintf(stderr, "Failed to get XSEL_DATA atom\n");
        return;
    }

    ret = get_atom("UTF8_STRING", &utf8_string_atom);
    if (ret) {
        fprintf(stderr, "Failed to get UTF8_STRING atom\n");
        return;
    }

    /*
     * Pack the data in our XSEL_DATA atom for safekeeping, generating an
     * XCB_SELECTION_NOTIFY event. Also, take ownership of the clipboard.
     */
    xcb_convert_selection(xcb_conn, evt_win, evt->selection, utf8_string_atom,
                          xsel_data_atom, XCB_CURRENT_TIME);
    xcb_set_selection_owner(xcb_conn, evt_win, evt->selection,
                            XCB_CURRENT_TIME);

    xcb_flush(xcb_conn);
#endif
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

        if (!evt || !evt->response_type) {
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
                handle_xfixes_selection_notify(evt);
            }
            break;
        }
    }
}

#define assert_streq(a, b)                                                     \
    if (strcmp((a), (b)) != 0) {                                               \
        fprintf(                                                               \
            stderr,                                                            \
            "selftest failed on line %d step '%s == %s', \"%s\" != \"%s\"\n",  \
            __LINE__, #a, #b, (a), (b));                                       \
        return -EINVAL;                                                        \
    }

#define assert_u32_eq(a, b)                                                    \
    if ((a) != (b)) {                                                          \
        fprintf(stderr,                                                        \
                "selftest failed on line %d step '%s == %s', \"%" PRIu32       \
                "\" != \"%" PRIu32 "\"\n",                                     \
                __LINE__, #a, #b, (a), (b));                                   \
        return -EINVAL;                                                        \
    }

static int selftest(void) {
    char *tmp;

    /* If it's all empty, the first line should also just be empty */
    tmp = get_first_line("");
    assert_streq(tmp, "");
    free(tmp);
    tmp = get_first_line("\n");
    assert_streq(tmp, "");
    free(tmp);

    /*
     * If all lines are empty, but there are N, result should be " (N lines)"
     */
    tmp = get_first_line("\n\n\n");
    assert_streq(tmp, " (3 lines)");
    free(tmp);

    /* If there's only one line, only that line should be displayed. */
    tmp = get_first_line("Foo bar\n");
    assert_streq(tmp, "Foo bar");
    free(tmp);

    /* If there's N lines, "(N lines)" should be displayed */
    tmp = get_first_line("Foo bar\nbaz\nqux\n");
    assert_streq(tmp, "Foo bar (3 lines)");
    free(tmp);

    /* If the last line didn't end with a newline, still count it */
    tmp = get_first_line("Foo bar");
    assert_streq(tmp, "Foo bar");
    free(tmp);
    tmp = get_first_line("Foo bar\nbaz");
    assert_streq(tmp, "Foo bar (2 lines)");
    free(tmp);

    /* UTF-8 tests */
    tmp = get_first_line("道");
    assert_streq(tmp, "道");
    free(tmp);
    tmp = get_first_line("道可到\n");
    assert_streq(tmp, "道可到");
    free(tmp);
    tmp = get_first_line("道可到\n非常道");
    assert_streq(tmp, "道可到 (2 lines)");
    free(tmp);

    /* Test djb2 hash */
    assert_u32_eq(djb2_hash("stottie"), (uint32_t)2933491793);
    assert_u32_eq(djb2_hash("肉夹馍"), (uint32_t)2954197494);
    assert_u32_eq(djb2_hash("пельме́ни"), (uint32_t)1457444436);

    return 0;
}

static void set_wm_attributes(const char *new_name) {
    xcb_generic_error_t *err;
    xcb_void_cookie_t cookie;

    cookie = xcb_change_property_checked(
        xcb_conn, XCB_PROP_MODE_REPLACE, evt_win, XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING, 8, strlen(new_name), new_name);
    err = xcb_request_check(xcb_conn, cookie);
    if (err) {
        fprintf(stderr, "Cannot set WM_CLASS\n");
    }

    cookie = xcb_change_property_checked(
        xcb_conn, XCB_PROP_MODE_REPLACE, evt_win, XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING, 8, strlen(new_name), new_name);
    err = xcb_request_check(xcb_conn, cookie);
    if (err) {
        fprintf(stderr, "Cannot set WM_NAME\n");
    }
}

int main(int argc, char *argv[]) {
    int err;

    if (argc == 2 && (strcmp(argv[1], "--test") == 0)) {
        err = selftest();
        if (!err) {
            printf("selftest completed successfully\n");
        }
        return err;
    }

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

    set_wm_attributes("clipmenud");

    event_loop();

    err = destroy_event_window();
    if (err) {
        xcb_disconnect(xcb_conn);
        die(3, "Failed to destroy event window\n");
    }

    xcb_disconnect(xcb_conn);
}
