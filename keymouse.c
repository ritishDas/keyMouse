#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

enum {
  FAST_STEP = 30,
  SLOW_STEP = 12,
};

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#define test_bit(bit, array)                                                   \
  ((array[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static int emit_event(int fd, unsigned short type, unsigned short code,
                      int value) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  gettimeofday(&ev.time, NULL);
  ev.type = type;
  ev.code = code;
  ev.value = value;
  return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int emit_syn(int fd) { return emit_event(fd, EV_SYN, SYN_REPORT, 0); }

static int emit_key(int fd, unsigned short code, int value) {
  if (emit_event(fd, EV_KEY, code, value) < 0)
    return -1;
  return emit_syn(fd);
}

static int emit_move(int fd, int dx, int dy) {
  if (dx != 0 && emit_event(fd, EV_REL, REL_X, dx) < 0)
    return -1;
  if (dy != 0 && emit_event(fd, EV_REL, REL_Y, dy) < 0)
    return -1;
  return emit_syn(fd);
}

static int emit_scroll(int fd, int clicks) {
  if (clicks != 0 && emit_event(fd, EV_REL, REL_WHEEL, clicks) < 0)
    return -1;
  return emit_syn(fd);
}

static int emit_click(int fd, int button_code) {
  if (emit_event(fd, EV_KEY, button_code, 1) < 0 || emit_syn(fd) < 0)
    return -1;
  usleep(25000); // 25ms hold time
  if (emit_event(fd, EV_KEY, button_code, 0) < 0 || emit_syn(fd) < 0)
    return -1;
  return 0;
}

static int setup_uinput_device(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0)
    return -1;

  ioctl(fd, UI_SET_EVBIT, EV_SYN);
  ioctl(fd, UI_SET_EVBIT, EV_REL);
  ioctl(fd, UI_SET_RELBIT, REL_X);
  ioctl(fd, UI_SET_RELBIT, REL_Y);
  ioctl(fd, UI_SET_RELBIT, REL_WHEEL);

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
  ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);

  for (int key = 1; key < KEY_MAX; key++) {
    ioctl(fd, UI_SET_KEYBIT, key);
  }

  struct uinput_setup usetup;
  memset(&usetup, 0, sizeof(usetup));
  snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "keyMouse controller");
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

static bool is_target_keyboard(int fd) {
  unsigned long ev_bits[NBITS(EV_MAX)] = {0};
  unsigned long key_bits[NBITS(KEY_MAX)] = {0};

  if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0 &&
      ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {

    if (test_bit(EV_KEY, ev_bits) && test_bit(KEY_A, key_bits) &&
        test_bit(KEY_ENTER, key_bits) && test_bit(KEY_SPACE, key_bits)) {

      char name[256] = "Unknown";
      ioctl(fd, EVIOCGNAME(sizeof(name)), name);

      if (strstr(name, "keyMouse") == NULL) {
        return true;
      }
    }
  }
  return false;
}

static int find_keyboard_device(char *out_path, size_t max_len) {
  DIR *dir = opendir("/dev/input");
  if (!dir)
    return -1;

  struct dirent *ent;
  int found_fd = -1;

  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, "event", 5) != 0)
      continue;

    char path[512];
    snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
      continue;

    if (is_target_keyboard(fd)) {
      char name[256] = "Unknown";
      ioctl(fd, EVIOCGNAME(sizeof(name)), name);
      snprintf(out_path, max_len, "%s", path);
      printf("Captured keyboard: %s (%s)\n", name, path);
      found_fd = fd;
      break;
    }
    close(fd);
  }
  closedir(dir);
  return found_fd;
}

static void drain_and_wait_key_release(int keyboard_fd) {
  unsigned long key_states[NBITS(KEY_MAX)] = {0};

  if (ioctl(keyboard_fd, EVIOCGKEY(sizeof(key_states)), key_states) >= 0) {
    while (test_bit(KEY_ENTER, key_states) ||
           test_bit(KEY_KPENTER, key_states)) {
      usleep(20000);
      if (ioctl(keyboard_fd, EVIOCGKEY(sizeof(key_states)), key_states) < 0)
        break;
    }
  }
  usleep(50000);
}

// Blocks until a keyboard device becomes available and opened
static int wait_for_keyboard(const char *target_path, char *out_path,
                             size_t max_len) {
  int inotify_fd = inotify_init();
  if (inotify_fd >= 0) {
    inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
  }

  while (1) {
    int fd = -1;
    if (target_path != NULL) {
      fd = open(target_path, O_RDONLY);
      if (fd >= 0) {
        snprintf(out_path, max_len, "%s", target_path);
      }
    } else {
      fd = find_keyboard_device(out_path, max_len);
    }

    if (fd >= 0) {
      if (inotify_fd >= 0)
        close(inotify_fd);
      return fd;
    }

    // Wait for changes in /dev/input instead of busy-spinning
    if (inotify_fd >= 0) {
      char buf[4096];
      (void)read(inotify_fd, buf, sizeof(buf));
    } else {
      sleep(1);
    }
  }
}

int main(int argc, char **argv) {
  const char *specific_device = NULL;
  char dev_path[512];

  if (argc >= 2 && strcmp(argv[1], "--help") != 0) {
    specific_device = argv[1];
  }

  int ui_fd = setup_uinput_device();
  if (ui_fd < 0) {
    fprintf(stderr, "Failed to create uinput device: %s\n", strerror(errno));
    return 1;
  }

  printf("\n--- Mode Ready ---\n");
  printf("• Hold LEFT ALT: Mouse mode\n");
  printf("  - H/J/K/L : Move cursor\n");
  printf("  - U / D   : Scroll Up / Down\n");
  printf("  - Enter   : Left Click\n");
  printf("  - Backspace: Right Click\n");
  printf("  - F       : Slow precision speed\n");
  printf("• RIGHT ALT: Regular Alt\n\n");

  // Reconnection recovery loop
  while (1) {
    int keyboard_fd =
        wait_for_keyboard(specific_device, dev_path, sizeof(dev_path));
    if (keyboard_fd < 0) {
      sleep(1);
      continue;
    }

    drain_and_wait_key_release(keyboard_fd);

    if (ioctl(keyboard_fd, EVIOCGRAB, 1) < 0) {
      fprintf(stderr, "Failed to grab keyboard exclusively: %s. Retrying...\n",
              strerror(errno));
      close(keyboard_fd);
      sleep(1);
      continue;
    }

    printf("Keyboard actively grabbed: %s\n", dev_path);

    bool left_alt_held = false;
    bool slow_held = false;
    bool super_held = false;
    bool ctrl_held = false;
    bool shift_held = false;
    struct input_event ev;

    // Process events until disconnect, read error, or emit failure
    while (1) {
      ssize_t n = read(keyboard_fd, &ev, sizeof(ev));
      if (n != (ssize_t)sizeof(ev)) {
        printf("\nKeyboard disconnected or read error (%s). Waiting for "
               "reconnect...\n",
               strerror(errno));
        break;
      }

      if (ev.type != EV_KEY) {
        continue;
      }

      bool pressed = (ev.value != 0);
      bool initial_press = (ev.value == 1);
      bool repeat = (ev.value == 2);

      // 1. Modifiers
      if (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA) {
        super_held = pressed;
        emit_key(ui_fd, ev.code, ev.value);
        continue;
      }
      if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
        ctrl_held = pressed;
        emit_key(ui_fd, ev.code, ev.value);
        continue;
      }
      if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
        shift_held = pressed;
        emit_key(ui_fd, ev.code, ev.value);
        continue;
      }

      // 2. Left Alt
      if (ev.code == KEY_LEFTALT) {
        if (super_held || ctrl_held || shift_held) {
          left_alt_held = false;
          emit_key(ui_fd, ev.code, ev.value);
          continue;
        }

        left_alt_held = pressed;
        if (!left_alt_held) {
          slow_held = false;
        }
        continue;
      }

      // 3. Normal typing mode
      if (!left_alt_held || super_held || ctrl_held || shift_held) {
        emit_key(ui_fd, ev.code, ev.value);
        continue;
      }

      // 4. Mouse mode
      if (ev.code == KEY_F) {
        slow_held = pressed;
        continue;
      }

      if (initial_press || repeat) {
        int step = slow_held ? SLOW_STEP : FAST_STEP;
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
        case KEY_U:
          rc = emit_scroll(ui_fd, 1);
          break;
        case KEY_D:
          rc = emit_scroll(ui_fd, -1);
          break;
        case KEY_ENTER:
        case KEY_KPENTER:
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
          fprintf(stderr, "Failed to emit uinput event: %s\n", strerror(errno));
          break;
        }
      }
    }

    // Clean up current disconnected keyboard and reset state
    ioctl(keyboard_fd, EVIOCGRAB, 0);
    close(keyboard_fd);
    usleep(200000); // 200ms debounce before re-polling
  }

  ioctl(ui_fd, UI_DEV_DESTROY);
  close(ui_fd);
  return 0;
}
