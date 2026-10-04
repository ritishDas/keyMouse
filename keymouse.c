#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    FAST_STEP = 12,
    SLOW_STEP = 3,
};

static int emit_event(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int emit_syn(int fd) {
    return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int emit_move(int fd, int dx, int dy) {
    if (dx != 0 && emit_event(fd, EV_REL, REL_X, dx) < 0) {
        return -1;
    }
    if (dy != 0 && emit_event(fd, EV_REL, REL_Y, dy) < 0) {
        return -1;
    }
    return emit_syn(fd);
}

static int emit_click(int fd, int button_code) {
    if (emit_event(fd, EV_KEY, button_code, 1) < 0 ||
        emit_event(fd, EV_KEY, button_code, 0) < 0) {
        return -1;
    }
    return emit_syn(fd);
}

static int setup_uinput_device(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_Y) < 0) {
        close(fd);
        return -1;
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "keyMouse virtual pointer");
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1;
    usetup.id.product = 0x1;
    usetup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s /dev/input/eventX\n", prog);
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return argc == 2 ? 0 : 1;
    }

    int keyboard_fd = open(argv[1], O_RDONLY);
    if (keyboard_fd < 0) {
        fprintf(stderr, "Failed to open keyboard device '%s': %s\n", argv[1], strerror(errno));
        return 1;
    }

    int ui_fd = setup_uinput_device();
    if (ui_fd < 0) {
        fprintf(stderr, "Failed to create uinput device: %s\n", strerror(errno));
        close(keyboard_fd);
        return 1;
    }

    bool alt_held = false;
    bool slow_held = false;
    struct input_event ev;

    while (read(keyboard_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY) {
            continue;
        }

        bool pressed = ev.value != 0;
        bool initial_press = ev.value == 1;
        bool repeat = ev.value == 2;
        int step = slow_held ? SLOW_STEP : FAST_STEP;

        if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
            alt_held = pressed;
            continue;
        }

        if (!alt_held) {
            continue;
        }

        if (ev.code == KEY_F) {
            slow_held = pressed;
            continue;
        }

        if (initial_press || repeat) {
            int rc = 0;
            switch (ev.code) {
                case KEY_H:
                    rc = emit_move(ui_fd, -step, 0);
                    break;
                case KEY_J:
                    rc = emit_move(ui_fd, 0, step);
                    break;
                case KEY_K:
                    rc = emit_move(ui_fd, 0, -step);
                    break;
                case KEY_L:
                    rc = emit_move(ui_fd, step, 0);
                    break;
                case KEY_ENTER:
                    if (initial_press) {
                        rc = emit_click(ui_fd, BTN_LEFT);
                    }
                    break;
                case KEY_BACKSPACE:
                    if (initial_press) {
                        rc = emit_click(ui_fd, BTN_RIGHT);
                    }
                    break;
                default:
                    break;
            }

            if (rc < 0) {
                fprintf(stderr, "Failed to emit event: %s\n", strerror(errno));
                break;
            }
        }
    }

    ioctl(ui_fd, UI_DEV_DESTROY);
    close(ui_fd);
    close(keyboard_fd);
    return 0;
}
