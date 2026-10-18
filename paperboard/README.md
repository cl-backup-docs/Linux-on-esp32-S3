# Paperboard JLX130-026 console

Target: SVERIO Paperboard v1, ESP32-S3 N16R8 and a JLX130-026 panel, a
1.3-inch 240 × 240 IPS TFT with the ST7789 controller on 4-wire SPI.
Linux runs on core 1; the ESP-IDF service and the display renderer stay on core 0.

## Wiring

| Panel pin | ESP32-S3 | Function |
| --- | --- | --- |
| CS  | GPIO10 | SPI chip select (active low) |
| RST | GPIO8  | reset (active low) |
| RS  | GPIO9  | data/command select |
| SDA | GPIO11 | SPI MOSI |
| SCK | GPIO12 | SPI clock |
| VDD | 3V3    | logic supply (2.4 – 4.6 V) |
| VSS | GND    | ground |
| LEDA | GPIO7 | backlight anode, driven high after init |

The controller-side pins are idle pins inside the former EPD data range, so
they can also be tapped at the Paperboard's EPD connector. Pins, SPI clock
(default 40 MHz) and rotation (0/90/180/270) are compile-time defaults at the
top of `st7789/include/st7789.h`; the terminal colors are in
`common/terminal.h` (white background, black text; swap the two RGB565 values
for a dark theme). If LEDA is tied to 3V3 instead, set `ST7789_PIN_BL` to -1.
No EPD power hardware (TPS65185 / PCA9555) is involved in this build.

## Architecture

Linux runs on core 1; the ESP-IDF service and display renderer stay on core 0.
A root-only `/dev/epd` misc device sends terminal bytes through the shared-memory
transport. `epd-shell` relays a PTY to the serial terminal and display. Root logins
on /dev/console or /dev/ttyS0 enter the relay automatically. Other terminals and
noninteractive invocations use the regular shell. Create /etc/paperboard-no-mirror
to disable automatic mirroring on future logins.

The display is a 30 × 15 ASCII terminal with cursor movement, scrolling, inverse
text and basic escape sequences. It is not a complete VT100 implementation and
does not mirror early boot messages. It keeps text states instead of allocating
a framebuffer in the PSRAM owned by Linux: every refresh, changed text rows are
converted to RGB565 scanlines in internal RAM and pushed to the panel through
SPI DMA, which takes milliseconds for a full screen.

The `st7789/` component is the panel driver (SPI setup, init sequence, windowed
writes, pixel byte order). `common/terminal.c` holds the terminal state machine
and the row renderer. The 8 × 16 bitmap font is Spleen 8x16
(https://github.com/fcambus/spleen, BSD-2-Clause, license text next to the
generated header); `tools/bdf-to-font.py` regenerates `common/font8x16_basic.h`
from the upstream BDF. A draw error stops updates until reboot.

## USB keyboard

A standard boot-protocol USB keyboard plugged into the **native USB port**
(GPIO19/20) types straight into the Linux console; no serial link is involved.
The firmware takes the USB PHY as a host (`linux/usb_kbd.c`, IDF `usb_host`
API), converts each report with `common/hid_keymap.c` (US layout, modifiers,
Caps Lock, 500 ms delay / 40 ms period auto-repeat, arrows and function keys as
terminal escape sequences) and sends the bytes as `ESP_KBD_IF` messages. The
kernel injects them into the ttyS0 input queue (`esp32_uart_inject_input()` in
`drivers/tty/serial/esp32_uart.c`), so the login prompt, `epd-shell` and the
mirrored panel session see them exactly as if they arrived on the serial line.
`[usb keyboard ready]` / `[usb keyboard removed]` notices appear on the panel
when the keyboard is plugged or unplugged.

Requirements and consequences:

- Use a USB-C male → USB-A female OTG adapter; the keyboard needs 5 V VBUS.
  If the keyboard LED stays dark, the board revision does not source VBUS on
  the native port — use a powered adapter or hub.
- The keyboard should offer the HID boot keyboard interface (class 3,
  subclass 1, protocol 0 or 1); the firmware asks for boot protocol if it is
  not already in it. Keyboards that hide behind an internal hub usually work,
  full composite devices may not.
- `/dev/pbkbd` (root only) mirrors the typed stream: reading it shows what the
  keyboard sent, writing to it injects bytes directly (a debug path).
- The 16 MB profile disables the USB-Serial/JTAG secondary console so the USB
  PHY is free for the keyboard: IDF logs stay on UART0 (the COM port), and
  flashing through the native USB port now needs BOOT+RESET instead of the
  automatic reset.

## Build and test

On a Linux build host with Docker, from a clean committed checkout:

    JOBS=8 bash build/reproduce.sh

Use `main` for the display build or `paperboard-headless` for the display-disabled
build. Use a fresh build directory when switching branches; do not reuse completed
stage markers from another variant. Build output includes a merged 16 MiB image
and a manifest with checksums and kernel/firmware vector compatibility checks.

Host tests (Clang, Python 3, ASan and UBSan):

    bash paperboard/tests/run.sh

## Verification

The ED097TC2 e-paper console reached V3, which was built and reported working
after V1/V2 had line queue underruns during Linux activity. V4 replaces that
output stage with the ST7789 driver described above; parser, renderer and
login-wrapper host tests pass, and the panel has been confirmed on real
hardware. The USB keyboard path is host-tested (`tests/test_hid_keymap.c`);
on-hardware verification is pending the first flash with keyboard support.
Headless V1 was built; the ELF flag and final rootfs were verified
to disable display initialization and automatic mirroring, respectively.

Headless retains the linked renderer to preserve the tested kernel memory layout,
but returns before display GPIO/I2C setup, power sequencing or task allocation.
/dev/epd remains available for ABI compatibility and discards writes in that build.

Factory images include public default credentials root / changeme123. Change the
password with passwd. Wi-Fi configuration is performed using the wifi command
from a serial shell, not Improv provisioning in the web installer.
