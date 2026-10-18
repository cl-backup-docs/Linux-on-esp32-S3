#include "hid_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
// Report maps trimmed down to what real HID keyboards send, plus a few
// shapes the parser must refuse.
// Standard six-key rollover keyboard, report ID 1.
static const uint8_t map_rid1[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, // Usage Page (GD), Usage (Keyboard), Collection (App)
    0x85, 0x01,                         //   Report ID (1)
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, //   Usage Page (Kbd), Usage Min/Max (0xE0..0xE7)
    0x15, 0x00, 0x25, 0x01,             //   Logical Min/Max (0..1)
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, //   Size 1, Count 8, Input (Data,Var,Abs)
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, //   Count 1, Size 8, Input (Const)
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, //   Usage Page (Kbd), Usage Min/Max (0..0x65)
    0x15, 0x00, 0x25, 0x65,             //   Logical Min/Max (0..0x65)
    0x95, 0x06, 0x75, 0x08, 0x81, 0x00, //   Count 6, Size 8, Input (Data,Array,Abs)
    0xC0,
};
// Same layout but without report IDs (the boot-protocol shape).
static const uint8_t map_bootlike[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65,
    0x95, 0x06, 0x75, 0x08, 0x81, 0x00,
    0xC0,
};
// A mouse (report ID 1) in front of a keyboard (report ID 3): the keyboard
// report must win even though the mouse report comes first.
static const uint8_t map_mouse_then_rid3[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, // mouse, Report ID (1)
    0x09, 0x01, 0xA1, 0x00,                         //   Pointer collection
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,             //   Usage Page (Button), 3 buttons
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,             //   padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, // X, Y, wheel
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x03, // keyboard, Report ID (3)
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65, 0x95, 0x06, 0x75, 0x08, 0x81, 0x00,
    0xC0,
};
// Modifiers described by an explicit usage list instead of a min/max pair.
static const uint8_t map_modlist[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x09, 0xE0, 0x09, 0xE1, 0x09, 0xE2, 0x09, 0xE3,
    0x09, 0xE4, 0x09, 0xE5, 0x09, 0xE6, 0x09, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65, 0x95, 0x06, 0x75, 0x08, 0x81, 0x00,
    0xC0,
};
// NKRO bitmap: no eight-bit usage slots, so it is not usable.
static const uint8_t map_bitmap_only[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x65, 0x81, 0x02,
    0xC0,
};
// Key array without any modifier byte: not usable either.
static const uint8_t map_keys_only[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
    0xC0,
};
// A short key array (four slots): usable, but the report is five bytes.
static const uint8_t map_short_keys[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x04, 0x81, 0x00,
    0xC0,
};
static PbHidKeyboardMap m;
static unsigned checks;
static void expect_parse(const uint8_t* map, size_t len, bool valid, uint8_t rid,
                         uint8_t input_len, uint8_t mod, uint8_t keys, uint8_t count) {
    checks++;
    bool ok = pb_hid_map_parse(map, len, &m);
    if (ok != valid || (ok && (m.report_id != rid || m.input_len != input_len ||
                               m.mod_offset != mod || m.keys_offset != keys ||
                               m.keys_count != count))) {
        fprintf(stderr,
                "check %u: parse=%d rid=%u len=%u mod=%u keys=%u count=%u\n",
                checks, ok, m.report_id, m.input_len, m.mod_offset, m.keys_offset,
                m.keys_count);
    }
    assert(ok == valid);
    if (valid)
        assert(m.report_id == rid && m.input_len == input_len &&
               m.mod_offset == mod && m.keys_offset == keys && m.keys_count == count);
}
static void expect_apply(const uint8_t* data, size_t len, bool valid,
                         const uint8_t expect[8]) {
    checks++;
    uint8_t out[PB_HID_REPORT_LEN] = {0};
    bool ok = pb_hid_map_apply(&m, data, len, out);
    if (ok != valid || (ok && memcmp(out, expect, 8))) {
        fprintf(stderr, "check %u: apply=%d:", checks, ok);
        for (unsigned i = 0; i < 8; i++)
            fprintf(stderr, " %02x", out[i]);
        fprintf(stderr, "\n");
    }
    assert(ok == valid);
    if (valid)
        assert(!memcmp(out, expect, 8));
}
int main(void) {
    expect_parse(map_rid1, sizeof(map_rid1), true, 1, 8, 0, 2, 6);
    expect_parse(map_bootlike, sizeof(map_bootlike), true, 0, 8, 0, 2, 6);
    expect_parse(map_mouse_then_rid3, sizeof(map_mouse_then_rid3), true, 3, 8, 0, 2, 6);
    expect_parse(map_modlist, sizeof(map_modlist), true, 2, 8, 0, 2, 6);
    expect_parse(map_bitmap_only, sizeof(map_bitmap_only), false, 0, 0, 0, 0, 0);
    expect_parse(map_keys_only, sizeof(map_keys_only), false, 0, 0, 0, 0, 0);
    expect_parse(map_short_keys, sizeof(map_short_keys), true, 1, 5, 0, 1, 4);
    // Truncated map: the parser stops, it does not read past the end.
    expect_parse(map_rid1, 20, false, 0, 0, 0, 0, 0);
    // The report ID byte may or may not be part of the characteristic value.
    const uint8_t with_id[] = {0x01, 0x02, 0x00, 0x04, 0, 0, 0, 0, 0};
    const uint8_t expect_shift_a[8] = {0x02, 0x00, 0x04, 0, 0, 0, 0, 0};
    assert(pb_hid_map_parse(map_rid1, sizeof(map_rid1), &m));
    expect_apply(with_id, sizeof(with_id), true, expect_shift_a);
    const uint8_t without_id[] = {0x02, 0x00, 0x04, 0, 0, 0, 0, 0};
    expect_apply(without_id, sizeof(without_id), true, expect_shift_a);
    // A payload whose first byte merely equals the report ID is not shifted.
    const uint8_t ctrl_only[] = {0x01, 0x00, 0x04, 0, 0, 0, 0, 0};
    expect_apply(ctrl_only, sizeof(ctrl_only), true, ctrl_only);
    // Six usable slots, no more.
    const uint8_t six_keys[] = {0x00, 0x00, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    expect_apply(six_keys, sizeof(six_keys), true, six_keys);
    expect_apply(six_keys, sizeof(six_keys) - 1, false, NULL);
    // Without a report ID in the map, the value is never shifted.
    assert(pb_hid_map_parse(map_bootlike, sizeof(map_bootlike), &m));
    expect_apply(six_keys, sizeof(six_keys), true, six_keys);
    // Four-slot map: the two missing slots stay idle.
    assert(pb_hid_map_parse(map_short_keys, sizeof(map_short_keys), &m));
    const uint8_t four_keys[] = {0x00, 0x04, 0x05, 0x06, 0x07};
    const uint8_t expect_four[8] = {0x00, 0x00, 0x04, 0x05, 0x06, 0x07, 0x00, 0x00};
    expect_apply(four_keys, sizeof(four_keys), true, expect_four);
    printf("hid_report: %u checks passed\n", checks);
    return 0;
}
