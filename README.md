hu# keyMouse

Keyboard-driven mouse mode for Linux using `uinput`.

## Build

```bash
make
```

## Run

```bash
sudo ./keymouse /dev/input/eventX
```

Replace `eventX` with your keyboard event device (for example from `sudo evtest`).

## Controls

- Hold `Alt` to enter mouse mode
- `h` / `j` / `k` / `l` move the pointer (vim motions)
- `u` for scroll up `d` for scroll down.
- Hold `f` while in mouse mode to slow pointer movement
- `Enter` performs left click
- `Backspace` performs right click
