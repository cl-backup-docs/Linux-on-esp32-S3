#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// USB HID boot-protocol keyboard reports (8 bytes: modifier byte, reserved,
// six usage slots) become the byte stream a serial terminal would send, so
// the bytes can be fed straight into a tty. US layout; Alt/Meta are not
// mapped. Used by the ESP-IDF side (linux/usb_kbd.c); pure C, host-tested.
#define PB_HID_REPORT_LEN 8
#define PB_KBD_REPEAT_DELAY_MS 500
#define PB_KBD_REPEAT_PERIOD_MS 40
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*pb_key_emit_fn)(void* ctx, const uint8_t* bytes, size_t len);
typedef struct {
    uint8_t keys[6];       // usages currently held (boot report order)
    uint8_t modifiers;     // HID modifier byte of the latest report
    bool caps_lock;
    uint8_t repeat_usage;  // usage that auto-repeats, 0 when none
    int64_t repeat_at_ms;  // deadline of the next auto-repeat
} PbKeyboard;
void pb_kbd_init(PbKeyboard* kb);
// Feed one report; keys pressed since the previous report are emitted.
void pb_kbd_report(PbKeyboard* kb, const uint8_t* report, int64_t now_ms,
                   pb_key_emit_fn emit, void* ctx);
// Call every few milliseconds; emits due auto-repeats.
void pb_kbd_tick(PbKeyboard* kb, int64_t now_ms, pb_key_emit_fn emit, void* ctx);
#ifdef __cplusplus
}
#endif
