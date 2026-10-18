#include "hid_keymap.h"
#include <string.h>
// Modifier bits (HID 1.11, 7.2.1): 0x01/0x10 Ctrl, 0x02/0x20 Shift.
#define MOD_CTRL 0x11u
#define MOD_SHIFT 0x22u
#define USAGE_ROLLOVER 0x01u
#define USAGE_CAPS_LOCK 0x39u
static bool held(const uint8_t* keys, uint8_t usage) {
    for (unsigned i = 0; i < 6; i++) if (keys[i] == usage) return true;
    return false;
}
static size_t seq(uint8_t* out, const char* s) {
    size_t n = strlen(s);
    memcpy(out, s, n);
    return n;
}
// Escape sequences match what a vt100-ish xterm sends, which is what readline
// and BusyBox on the serial console already understand.
static size_t usage_bytes(const PbKeyboard* kb, uint8_t usage, uint8_t* out) {
    bool ctrl = (kb->modifiers & MOD_CTRL) != 0;
    bool shift = (kb->modifiers & MOD_SHIFT) != 0;
    if (usage >= 0x04 && usage <= 0x1d) {              // a .. z
        uint8_t ch = (uint8_t)('a' + usage - 0x04);
        if (ctrl) { out[0] = (uint8_t)(ch & 0x1f); return 1; }
        if (shift != kb->caps_lock) ch = (uint8_t)(ch - 'a' + 'A');
        out[0] = ch;
        return 1;
    }
    if (usage >= 0x1e && usage <= 0x27) {              // 1 .. 9, 0
        static const char plain[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        static const uint8_t ctrl_bits[] = {0, 0, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0, 0, 0}; // ctrl-3 .. ctrl-7
        if (ctrl) {
            if (!ctrl_bits[usage - 0x1e]) return 0;
            out[0] = ctrl_bits[usage - 0x1e];
            return 1;
        }
        out[0] = (uint8_t)(shift ? shifted : plain)[usage - 0x1e];
        return 1;
    }
    switch (usage) {
    case 0x28: out[0] = '\r'; return 1;
    case 0x29: out[0] = 0x1b; return 1;
    case 0x2a: out[0] = (uint8_t)(ctrl ? 0x08 : 0x7f); return 1;   // ctrl-h or DEL
    case 0x2b: out[0] = '\t'; return 1;
    case 0x2c: out[0] = ' '; return 1;
    case 0x2d: if (ctrl) { out[0] = 0x1f; return 1; } out[0] = (uint8_t)(shift ? '_' : '-'); return 1;
    case 0x2e: out[0] = (uint8_t)(shift ? '+' : '='); return 1;
    case 0x2f: if (ctrl) { out[0] = 0x1b; return 1; } out[0] = (uint8_t)(shift ? '{' : '['); return 1;
    case 0x30: if (ctrl) { out[0] = 0x1d; return 1; } out[0] = (uint8_t)(shift ? '}' : ']'); return 1;
    case 0x31: if (ctrl) { out[0] = 0x1c; return 1; } out[0] = (uint8_t)(shift ? '|' : '\\'); return 1;
    case 0x33: out[0] = (uint8_t)(shift ? ':' : ';'); return 1;
    case 0x34: out[0] = (uint8_t)(shift ? '"' : '\''); return 1;
    case 0x35: out[0] = (uint8_t)(shift ? '~' : '`'); return 1;
    case 0x36: out[0] = (uint8_t)(shift ? '<' : ','); return 1;
    case 0x37: out[0] = (uint8_t)(shift ? '>' : '.'); return 1;
    case 0x38: if (ctrl) { out[0] = (uint8_t)(shift ? 0x7f : 0x1f); return 1; }
               out[0] = (uint8_t)(shift ? '?' : '/'); return 1;
    case 0x3a: return seq(out, "\x1bOP");              // F1 .. F12
    case 0x3b: return seq(out, "\x1bOQ");
    case 0x3c: return seq(out, "\x1bOR");
    case 0x3d: return seq(out, "\x1bOS");
    case 0x3e: return seq(out, "\x1b[15~");
    case 0x3f: return seq(out, "\x1b[17~");
    case 0x40: return seq(out, "\x1b[18~");
    case 0x41: return seq(out, "\x1b[19~");
    case 0x42: return seq(out, "\x1b[20~");
    case 0x43: return seq(out, "\x1b[21~");
    case 0x44: return seq(out, "\x1b[23~");
    case 0x45: return seq(out, "\x1b[24~");
    case 0x49: return seq(out, "\x1b[2~");             // Insert
    case 0x4a: return seq(out, "\x1b[H");              // Home
    case 0x4b: return seq(out, "\x1b[5~");             // PageUp
    case 0x4c: return seq(out, "\x1b[3~");             // Delete
    case 0x4d: return seq(out, "\x1b[F");              // End
    case 0x4e: return seq(out, "\x1b[6~");             // PageDown
    case 0x4f: return seq(out, "\x1b[C");              // Right
    case 0x50: return seq(out, "\x1b[D");              // Left
    case 0x51: return seq(out, "\x1b[B");              // Down
    case 0x52: return seq(out, "\x1b[A");              // Up
    default: return 0;
    }
}
static void emit_usage(const PbKeyboard* kb, uint8_t usage, pb_key_emit_fn emit, void* ctx) {
    uint8_t buf[8];
    size_t n = usage_bytes(kb, usage, buf);
    if (n) emit(ctx, buf, n);
}
void pb_kbd_init(PbKeyboard* kb) { memset(kb, 0, sizeof(*kb)); }
void pb_kbd_report(PbKeyboard* kb, const uint8_t* report, int64_t now_ms,
                   pb_key_emit_fn emit, void* ctx) {
    uint8_t next[6];
    memcpy(next, report + 2, 6);
    kb->modifiers = report[0];
    for (unsigned i = 0; i < 6; i++) {
        uint8_t usage = next[i];
        if (!usage || usage == USAGE_ROLLOVER || held(kb->keys, usage)) continue;
        if (usage == USAGE_CAPS_LOCK) { kb->caps_lock = !kb->caps_lock; continue; }
        emit_usage(kb, usage, emit, ctx);
        kb->repeat_usage = usage;
        kb->repeat_at_ms = now_ms + PB_KBD_REPEAT_DELAY_MS;
    }
    for (unsigned i = 0; i < 6; i++) {
        if (kb->keys[i] && kb->keys[i] == kb->repeat_usage && !held(next, kb->keys[i]))
            kb->repeat_usage = 0;
    }
    memcpy(kb->keys, next, 6);
}
void pb_kbd_tick(PbKeyboard* kb, int64_t now_ms, pb_key_emit_fn emit, void* ctx) {
    if (!kb->repeat_usage || now_ms < kb->repeat_at_ms) return;
    emit_usage(kb, kb->repeat_usage, emit, ctx);
    kb->repeat_at_ms = now_ms + PB_KBD_REPEAT_PERIOD_MS;
}
