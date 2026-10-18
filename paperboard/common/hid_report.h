#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hid_keymap.h"
// BLE HOGP keyboards describe their input reports with a "report map" (a HID
// report descriptor) that the host reads from the device. This parser finds
// the keyboard application collection's modifier byte and key array so a
// Report characteristic notification can be turned into the 8-byte boot
// layout hid_keymap.c consumes. Pure C, host-tested. Not reentrant: it keeps
// one parser instance (no large stack frame), used from the NimBLE host task
// only.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool valid;
    uint8_t report_id;   // 0 when the device's reports carry no report ID byte
    uint8_t input_len;   // input report length in bytes, excluding the ID byte
    uint8_t mod_offset;  // byte offset of the modifier bits
    uint8_t keys_offset; // byte offset of the first 8-bit usage slot
    uint8_t keys_count;  // usage slots in the key array (capped at six)
} PbHidKeyboardMap;
// Parses a report map; false when it holds no usable keyboard input report.
bool pb_hid_map_parse(const uint8_t* map, size_t len, PbHidKeyboardMap* out);
// Applies the map to a Report characteristic payload, producing the 8-byte
// boot layout [modifiers, 0, k0..k5]. Payloads that carry a leading report ID
// byte (the stacks disagree on whether the ID is part of the value) are
// accepted too. False when the payload is shorter than the report.
bool pb_hid_map_apply(const PbHidKeyboardMap* m, const uint8_t* data, size_t len,
                      uint8_t out[PB_HID_REPORT_LEN]);
#ifdef __cplusplus
}
#endif
