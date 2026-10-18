#include "hid_report.h"
#include <string.h>
// HID 1.11 section 6.2.2: an item is a one-byte prefix [tag:4 type:2 size:2]
// followed by size bytes of little-endian data (size 3 means four bytes).
enum { TYPE_MAIN = 0, TYPE_GLOBAL = 1, TYPE_LOCAL = 2 };
enum {
    MAIN_INPUT = 0x8, MAIN_COLLECTION = 0xA, MAIN_END_COLLECTION = 0xC,
    GLOBAL_USAGE_PAGE = 0x0, GLOBAL_REPORT_SIZE = 0x7, GLOBAL_REPORT_ID = 0x8,
    GLOBAL_REPORT_COUNT = 0x9,
    LOCAL_USAGE = 0x0, LOCAL_USAGE_MIN = 0x1, LOCAL_USAGE_MAX = 0x2,
};
enum { PAGE_GENERIC_DESKTOP = 0x01, PAGE_KEYBOARD = 0x07, USAGE_KEYBOARD = 0x06 };
#define COLLECTION_APPLICATION 0x01u
#define MOD_USAGE_MIN 0xE0u
#define MOD_USAGE_MAX 0xE7u
#define MAX_SLOTS 6 // boot-layout usage slots hid_keymap.c reads
typedef struct {
    uint16_t page;
    uint16_t id;
} Usage;
typedef struct {
    // global item state (survives main items)
    uint32_t usage_page;
    uint8_t report_id;
    uint32_t report_size;
    uint32_t report_count;
    // local item state (cleared by every main item)
    Usage usages[8];
    unsigned num_usages;
    Usage usage; // last single Usage item, used by Collection
    bool have_usage;
    Usage usage_min, usage_max;
    bool have_min, have_max;
    // collection tracking
    int depth;
    bool in_keyboard;
    int keyboard_depth;
    // results, indexed by report ID
    uint16_t in_bits[256]; // input bits accumulated so far per report ID
    uint8_t mod_off[256], keys_off[256], keys_n[256];
    bool mod_found[256], keys_found[256];
} ReportMapParser;
static ReportMapParser parser;
static uint32_t item_data(const uint8_t* p, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}
// A four-byte usage item carries the usage page in its high half; shorter
// items use the current (global) usage page.
static Usage usage_of(uint32_t data, unsigned size, uint32_t page) {
    Usage u;
    u.page = (size == 4) ? (uint16_t)(data >> 16) : (uint16_t)page;
    u.id = (uint16_t)data;
    return u;
}
static bool is_keyboard_application(const ReportMapParser* p, uint32_t collection_type) {
    if (collection_type != COLLECTION_APPLICATION || !p->have_usage)
        return false;
    if (p->usage.id != USAGE_KEYBOARD)
        return false;
    return p->usage.page == PAGE_GENERIC_DESKTOP || p->usage.page == PAGE_KEYBOARD;
}
// Eight one-bit modifier fields: either Usage Minimum/Maximum 0xE0/0xE7 or an
// explicit usage list that covers all eight codes.
static bool is_modifier_set(const ReportMapParser* p) {
    if (p->have_min && p->have_max &&
        p->usage_min.page == PAGE_KEYBOARD && p->usage_max.page == PAGE_KEYBOARD &&
        p->usage_min.id == MOD_USAGE_MIN && p->usage_max.id == MOD_USAGE_MAX)
        return true;
    unsigned n = 0;
    for (unsigned i = 0; i < p->num_usages; i++)
        if (p->usages[i].page == PAGE_KEYBOARD &&
            p->usages[i].id >= MOD_USAGE_MIN && p->usages[i].id <= MOD_USAGE_MAX)
            n++;
    return n >= 8;
}
// The key array covers usages on the Keyboard/Keypad page.
static bool is_key_usage_set(const ReportMapParser* p) {
    if (p->have_min)
        return p->usage_min.page == PAGE_KEYBOARD;
    for (unsigned i = 0; i < p->num_usages; i++)
        if (p->usages[i].page == PAGE_KEYBOARD)
            return true;
    return p->usage_page == PAGE_KEYBOARD;
}
bool pb_hid_map_parse(const uint8_t* map, size_t len, PbHidKeyboardMap* out) {
    if (!map || !out)
        return false;
    ReportMapParser* p = &parser;
    memset(p, 0, sizeof(*p));
    size_t i = 0;
    while (i < len) {
        uint8_t prefix = map[i++];
        if (prefix == 0xFE) { // long item: [size][tag][data...]
            if (i + 2 > len)
                break;
            unsigned long_size = map[i];
            i += 2 + long_size;
            continue;
        }
        unsigned size = prefix & 0x03;
        if (size == 3)
            size = 4;
        unsigned type = (prefix >> 2) & 0x03;
        unsigned tag = prefix >> 4;
        if (i + size > len)
            break;
        uint32_t data = item_data(map + i, size);
        i += size;
        if (type == TYPE_GLOBAL) {
            switch (tag) {
            case GLOBAL_USAGE_PAGE: p->usage_page = data; break;
            case GLOBAL_REPORT_SIZE: p->report_size = data; break;
            case GLOBAL_REPORT_ID: p->report_id = (uint8_t)data; break;
            case GLOBAL_REPORT_COUNT: p->report_count = data; break;
            default: break;
            }
            continue;
        }
        if (type == TYPE_LOCAL) {
            switch (tag) {
            case LOCAL_USAGE:
                p->usage = usage_of(data, size, p->usage_page);
                p->have_usage = true;
                if (p->num_usages < 8)
                    p->usages[p->num_usages++] = p->usage;
                break;
            case LOCAL_USAGE_MIN:
                p->usage_min = usage_of(data, size, p->usage_page);
                p->have_min = true;
                break;
            case LOCAL_USAGE_MAX:
                p->usage_max = usage_of(data, size, p->usage_page);
                p->have_max = true;
                break;
            default: break;
            }
            continue;
        }
        if (type != TYPE_MAIN)
            continue;
        switch (tag) {
        case MAIN_COLLECTION:
            p->depth++;
            if (!p->in_keyboard && is_keyboard_application(p, data)) {
                p->in_keyboard = true;
                p->keyboard_depth = p->depth;
            }
            break;
        case MAIN_END_COLLECTION:
            if (p->in_keyboard && p->depth == p->keyboard_depth)
                p->in_keyboard = false;
            if (p->depth > 0)
                p->depth--;
            break;
        case MAIN_INPUT:
            if (p->report_size && p->report_count) {
                uint32_t bits = p->report_size * p->report_count;
                uint32_t off = p->in_bits[p->report_id];
                if (p->in_keyboard && (off & 7) == 0 && bits <= 0xFFFF) {
                    // Input item flags: bit 0 is 0 for Data / 1 for Constant,
                    // bit 1 is 0 for Array / 1 for Variable.
                    bool is_data = (data & 0x01) == 0;
                    bool is_array = (data & 0x02) == 0;
                    if (is_data && !is_array && p->report_size == 1 &&
                        p->report_count == 8 && is_modifier_set(p)) {
                        if (!p->mod_found[p->report_id]) {
                            p->mod_found[p->report_id] = true;
                            p->mod_off[p->report_id] = (uint8_t)(off >> 3);
                        }
                    } else if (is_data && is_array && p->report_size == 8 &&
                               p->report_count >= 1 && is_key_usage_set(p)) {
                        unsigned n = p->report_count > MAX_SLOTS ? MAX_SLOTS : p->report_count;
                        if (!p->keys_found[p->report_id] ||
                            (p->keys_n[p->report_id] < MAX_SLOTS && n > p->keys_n[p->report_id])) {
                            p->keys_found[p->report_id] = true;
                            p->keys_off[p->report_id] = (uint8_t)(off >> 3);
                            p->keys_n[p->report_id] = (uint8_t)n;
                        }
                    }
                }
                if (bits <= 0xFFFF - off)
                    p->in_bits[p->report_id] = (uint16_t)(off + bits);
            }
            break;
        default:
            break;
        }
        p->num_usages = 0;
        p->have_usage = false;
        p->have_min = p->have_max = false;
    }
    out->valid = false;
    for (unsigned rid = 0; rid < 256; rid++) {
        if (!p->mod_found[rid] || !p->keys_found[rid])
            continue;
        if (p->keys_off[rid] == p->mod_off[rid])
            continue;
        unsigned input_len = (p->in_bits[rid] + 7u) / 8u;
        if (input_len == 0 || input_len > 255)
            continue;
        if (p->mod_off[rid] >= input_len || p->keys_off[rid] >= input_len)
            continue;
        out->valid = true;
        out->report_id = (uint8_t)rid;
        out->input_len = (uint8_t)input_len;
        out->mod_offset = p->mod_off[rid];
        out->keys_offset = p->keys_off[rid];
        out->keys_count = p->keys_n[rid];
        return true;
    }
    return false;
}
bool pb_hid_map_apply(const PbHidKeyboardMap* m, const uint8_t* data, size_t len,
                      uint8_t out[PB_HID_REPORT_LEN]) {
    if (!m || !m->valid || !data || !out)
        return false;
    // A payload one byte longer than the report can only be the report with
    // its ID byte prepended; reports never carry bytes their map lacks.
    size_t off = 0;
    if (m->report_id != 0 && len == (size_t)m->input_len + 1 && data[0] == m->report_id)
        off = 1;
    if (len < off + m->input_len)
        return false;
    memset(out, 0, PB_HID_REPORT_LEN);
    out[0] = data[off + m->mod_offset];
    for (unsigned i = 0; i < MAX_SLOTS; i++)
        if (i < m->keys_count)
            out[2 + i] = data[off + m->keys_offset + i];
    return true;
}
