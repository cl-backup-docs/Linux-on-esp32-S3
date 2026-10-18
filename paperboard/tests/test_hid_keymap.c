#include "hid_keymap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t out[256];
static size_t out_len;
static void emit(void* ctx, const uint8_t* bytes, size_t len) {
    (void)ctx;
    assert(out_len + len <= sizeof(out));
    memcpy(out + out_len, bytes, len);
    out_len += len;
}
static unsigned checks;
static void expect(const char* bytes, size_t len) {
    checks++;
    if (out_len != len || (len != 0 && memcmp(out, bytes, len))) {
        fprintf(stderr, "check %u: expected %zu bytes:", checks, len);
        for (size_t i = 0; i < len; i++) fprintf(stderr, " %02x", (unsigned char)bytes[i]);
        fprintf(stderr, "; got %zu bytes:", out_len);
        for (size_t i = 0; i < out_len; i++) fprintf(stderr, " %02x", out[i]);
        fprintf(stderr, "\n");
    }
    assert(out_len == len && (len == 0 || !memcmp(out, bytes, len)));
    out_len = 0;
}
static void feed(PbKeyboard* kb, uint8_t mods, uint8_t k0, int64_t now) {
    uint8_t r[PB_HID_REPORT_LEN] = {mods, 0, k0, 0, 0, 0, 0, 0};
    pb_kbd_report(kb, r, now, emit, NULL);
}
int main(void) {
    PbKeyboard kb;
    pb_kbd_init(&kb);
    // Letters, shift and caps lock.
    feed(&kb, 0, 0x04, 0); expect("a", 1);
    feed(&kb, 0, 0, 30);
    feed(&kb, 0x02, 0x04, 60); expect("A", 1);
    feed(&kb, 0x22, 0x04, 90); expect("", 0);             // both shifts, held
    feed(&kb, 0, 0, 120);
    feed(&kb, 0, 0x39, 150); expect("", 0);               // caps on
    feed(&kb, 0, 0x04, 180); expect("A", 1);
    feed(&kb, 0x02, 0x04, 210); expect("", 0);            // shift while held
    feed(&kb, 0x02, 0, 215); expect("", 0);               // release a, keep shift
    feed(&kb, 0x02, 0x04, 220); expect("a", 1);           // caps+shift
    feed(&kb, 0, 0x39, 240); expect("", 0);               // caps off
    feed(&kb, 0, 0x04, 270); expect("a", 1);
    feed(&kb, 0, 0, 300);
    // Letters that change while held (e.g. over a compiled-in repeat rate)
    // keep their repeat; the deadline is refreshed only on a new press.
    feed(&kb, 0, 0x15, 330); expect("r", 1);
    pb_kbd_tick(&kb, 829, emit, NULL); expect("", 0);
    pb_kbd_tick(&kb, 830, emit, NULL); expect("r", 1);    // 500 ms after press
    pb_kbd_tick(&kb, 869, emit, NULL); expect("", 0);
    pb_kbd_tick(&kb, 870, emit, NULL); expect("r", 1);    // 40 ms period
    feed(&kb, 0, 0, 900);                                 // release stops it
    pb_kbd_tick(&kb, 1400, emit, NULL); expect("", 0);
    // Ctrl codes, including the classic ones a console user needs.
    feed(&kb, 0x01, 0x06, 1000); expect("\x03", 1);       // ctrl+c
    feed(&kb, 0, 0, 1030);
    feed(&kb, 0x01, 0x29, 1060); expect("\x1b", 1);       // ctrl+[ = ESC
    feed(&kb, 0, 0, 1090);
    feed(&kb, 0x01, 0x2f, 1120); expect("\x1b", 1);
    feed(&kb, 0, 0, 1150);
    feed(&kb, 0x01, 0x31, 1180); expect("\x1c", 1);
    feed(&kb, 0, 0, 1210);
    feed(&kb, 0x01, 0x30, 1240); expect("\x1d", 1);
    feed(&kb, 0, 0, 1270);
    feed(&kb, 0x01, 0x20, 1300); expect("\x1b", 1);       // ctrl+3
    feed(&kb, 0, 0, 1330);
    feed(&kb, 0x01, 0x2d, 1360); expect("\x1f", 1);       // ctrl+-
    feed(&kb, 0, 0, 1390);
    feed(&kb, 0x01, 0x2a, 1420); expect("\x08", 1);       // ctrl+backspace
    feed(&kb, 0, 0, 1450);
    feed(&kb, 0x01, 0x38, 1480); expect("\x1f", 1);       // ctrl+/
    feed(&kb, 0, 0, 1510);
    feed(&kb, 0x03, 0x38, 1540); expect("\x7f", 1);       // ctrl+?
    feed(&kb, 0, 0, 1570);
    // Plain keys.
    feed(&kb, 0, 0x28, 1600); expect("\r", 1);
    feed(&kb, 0, 0, 1630);
    feed(&kb, 0, 0x2a, 1660); expect("\x7f", 1);
    feed(&kb, 0, 0, 1690);
    feed(&kb, 0, 0x2b, 1720); expect("\t", 1);
    feed(&kb, 0, 0, 1750);
    feed(&kb, 0, 0x2c, 1780); expect(" ", 1);
    feed(&kb, 0, 0, 1810);
    feed(&kb, 0, 0x29, 1840); expect("\x1b", 1);
    feed(&kb, 0, 0, 1870);
    // Shifted punctuation.
    feed(&kb, 0x02, 0x1e, 1900); expect("!", 1);
    feed(&kb, 0x02, 0x27, 1930); expect(")", 1);
    feed(&kb, 0x02, 0x24, 1960); expect("&", 1);
    feed(&kb, 0, 0, 1980);
    feed(&kb, 0, 0x2d, 1990); expect("-", 1);
    feed(&kb, 0, 0, 1995);
    feed(&kb, 0x02, 0x2d, 2020); expect("_", 1);
    feed(&kb, 0, 0, 2035);
    feed(&kb, 0, 0x2e, 2050); expect("=", 1);
    feed(&kb, 0, 0, 2065);
    feed(&kb, 0x02, 0x2e, 2080); expect("+", 1);
    feed(&kb, 0, 0, 2095);
    feed(&kb, 0, 0x2f, 2110); expect("[", 1);
    feed(&kb, 0, 0, 2125);
    feed(&kb, 0x02, 0x2f, 2140); expect("{", 1);
    feed(&kb, 0, 0, 2155);
    feed(&kb, 0, 0x30, 2170); expect("]", 1);
    feed(&kb, 0, 0, 2185);
    feed(&kb, 0x02, 0x30, 2200); expect("}", 1);
    feed(&kb, 0, 0, 2215);
    feed(&kb, 0, 0x31, 2230); expect("\\", 1);
    feed(&kb, 0, 0, 2245);
    feed(&kb, 0x02, 0x31, 2260); expect("|", 1);
    feed(&kb, 0, 0, 2275);
    feed(&kb, 0, 0x33, 2290); expect(";", 1);
    feed(&kb, 0, 0, 2305);
    feed(&kb, 0x02, 0x33, 2320); expect(":", 1);
    feed(&kb, 0, 0, 2335);
    feed(&kb, 0, 0x34, 2350); expect("'", 1);
    feed(&kb, 0, 0, 2365);
    feed(&kb, 0x02, 0x34, 2380); expect("\"", 1);
    feed(&kb, 0, 0, 2395);
    feed(&kb, 0, 0x35, 2410); expect("`", 1);
    feed(&kb, 0, 0, 2425);
    feed(&kb, 0x02, 0x35, 2440); expect("~", 1);
    feed(&kb, 0, 0, 2455);
    feed(&kb, 0, 0x36, 2470); expect(",", 1);
    feed(&kb, 0, 0, 2485);
    feed(&kb, 0x02, 0x36, 2500); expect("<", 1);
    feed(&kb, 0, 0, 2515);
    feed(&kb, 0, 0x37, 2530); expect(".", 1);
    feed(&kb, 0, 0, 2545);
    feed(&kb, 0x02, 0x37, 2560); expect(">", 1);
    feed(&kb, 0, 0, 2575);
    feed(&kb, 0, 0x38, 2590); expect("/", 1);
    feed(&kb, 0, 0, 2605);
    feed(&kb, 0x02, 0x38, 2620); expect("?", 1);
    feed(&kb, 0, 0, 2650);
    // Navigation, editing and function keys.
    feed(&kb, 0, 0x4f, 2700); expect("\x1b[C", 3);
    feed(&kb, 0, 0, 2730);
    feed(&kb, 0, 0x50, 2760); expect("\x1b[D", 3);
    feed(&kb, 0, 0, 2790);
    feed(&kb, 0, 0x51, 2820); expect("\x1b[B", 3);
    feed(&kb, 0, 0, 2850);
    feed(&kb, 0, 0x52, 2880); expect("\x1b[A", 3);
    feed(&kb, 0, 0, 2910);
    feed(&kb, 0, 0x4a, 2940); expect("\x1b[H", 3);
    feed(&kb, 0, 0, 2970);
    feed(&kb, 0, 0x4d, 3000); expect("\x1b[F", 3);
    feed(&kb, 0, 0, 3030);
    feed(&kb, 0, 0x49, 3060); expect("\x1b[2~", 4);
    feed(&kb, 0, 0, 3090);
    feed(&kb, 0, 0x4c, 3120); expect("\x1b[3~", 4);
    feed(&kb, 0, 0, 3150);
    feed(&kb, 0, 0x4b, 3180); expect("\x1b[5~", 4);
    feed(&kb, 0, 0, 3210);
    feed(&kb, 0, 0x4e, 3240); expect("\x1b[6~", 4);
    feed(&kb, 0, 0, 3270);
    feed(&kb, 0, 0x3a, 3300); expect("\x1bOP", 3);       // F1
    feed(&kb, 0, 0, 3330);
    feed(&kb, 0, 0x3e, 3360); expect("\x1b[15~", 5);     // F5
    feed(&kb, 0, 0, 3390);
    feed(&kb, 0, 0x45, 3420); expect("\x1b[24~", 5);     // F12
    feed(&kb, 0, 0, 3450);
    // Two keys in one report both emit; repeat follows the later slot.
    {
        uint8_t r[PB_HID_REPORT_LEN] = {0, 0, 0x04, 0x05, 0, 0, 0, 0};
        pb_kbd_report(&kb, r, 3500, emit, NULL);
        expect("ab", 2);
        pb_kbd_tick(&kb, 4000, emit, NULL); expect("b", 1);
        uint8_t r2[PB_HID_REPORT_LEN] = {0, 0, 0x04, 0, 0, 0, 0, 0};
        pb_kbd_report(&kb, r2, 4030, emit, NULL);         // release b, hold a
        expect("", 0);
        pb_kbd_tick(&kb, 4500, emit, NULL); expect("", 0);
        pb_kbd_report(&kb, (const uint8_t[]){0, 0, 0, 0, 0, 0, 0, 0}, 5000, emit, NULL);
    }
    // A press of the same key while already held repeats nothing.
    feed(&kb, 0, 0x04, 5100); expect("a", 1);
    feed(&kb, 0, 0x04, 5130); expect("", 0);
    feed(&kb, 0, 0, 5160);
    // Unmapped usages and empty slots are ignored.
    feed(&kb, 0, 0x00, 5200); expect("", 0);
    feed(&kb, 0, 0x01, 5230); expect("", 0);              // rollover
    feed(&kb, 0, 0x63, 5260); expect("", 0);
    feed(&kb, 0, 0, 5290);
    // Random reports and ticks stay in bounds (run with ASan/UBSan).
    unsigned seed = 7;
    for (unsigned i = 0; i < 200000; i++) {
        uint8_t r[PB_HID_REPORT_LEN];
        for (unsigned j = 0; j < PB_HID_REPORT_LEN; j++) {
            seed = 1664525 * seed + 1013904223;
            r[j] = (uint8_t)(seed >> 24);
        }
        int64_t now = (int64_t)(seed & 0xffff);
        pb_kbd_report(&kb, r, now, emit, NULL);
        if (seed & 1) pb_kbd_tick(&kb, now, emit, NULL);
        out_len = 0;
    }
    printf("hid keymap tests passed; state: %zu bytes\n", sizeof(PbKeyboard));
    return 0;
}
