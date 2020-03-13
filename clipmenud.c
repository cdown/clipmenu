/* gcc clipmenud.c -lxcb -o cd */

#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>

#define die(status, ...)                                                       \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(status);

int main(int argc, char *argv[]) {
    xcb_connection_t *xcb;

    xcb = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(xcb)) {
        xcb_disconnect(xcb);
        die(1, "Failed to connect to X server\n");
    }
}
