#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Text grid of the JLX130-026 panel (ST7789, 240 x 240): 30 columns x 15 rows
// of 8 x 16 pixel cells. The Linux-side ABI (/dev/epd, epd-shell) is unchanged.
#define PB_COLS 30
#define PB_ROWS 15
#define PB_CELL_W 8
#define PB_CELL_H 16
#define PB_WIDTH (PB_COLS*PB_CELL_W)
#define PB_HEIGHT (PB_ROWS*PB_CELL_H)
// RGB565 terminal colors; swap the two values for a light-on-dark theme.
#define PB_COLOR_BG 0xffffu
#define PB_COLOR_FG 0x0000u
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint8_t ch, inverse; } PbCell;
typedef struct {
    PbCell cells[PB_ROWS][PB_COLS];
    unsigned x, y, saved_x, saved_y, state, nparam, params[8];
    bool inverse, wrap_pending, cursor_visible, private_seq;
} PbTerminal;
void pb_init(PbTerminal* t);
void pb_feed(PbTerminal* t, const uint8_t* data, size_t len);
// Renders one text row (PB_CELL_H scanlines of PB_WIDTH RGB565 pixels each).
void pb_render_cell_row(const PbTerminal* t, unsigned row, uint16_t* pixels);
// Rows whose cells or cursor differ between next and shown.
void pb_dirty_rows(const PbTerminal* next, const PbTerminal* shown, bool dirty[PB_ROWS]);
#ifdef __cplusplus
}
#endif
