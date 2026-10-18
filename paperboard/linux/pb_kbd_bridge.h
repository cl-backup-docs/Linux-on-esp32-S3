#pragma once
#include <stddef.h>
#include <stdint.h>
// Keyboard bytes travel to Linux as ESP_KBD_IF messages. Both keyboard
// front-ends -- the USB host driver (usb_kbd.c) and the BLE HOGP client
// (ble_kbd.c) -- share this sink, so the two keyboards stay interchangeable
// to the Linux side (ttyS0 and /dev/pbkbd).
#ifdef __cplusplus
extern "C" {
#endif
// Sends one message; nonzero when the host link is busy or unavailable.
int pb_kbd_send(const uint8_t* bytes, size_t len);
// Appends a status line to the panel console that mirrors the login.
void pb_kbd_notice(const char* text);
#ifdef __cplusplus
}
#endif
