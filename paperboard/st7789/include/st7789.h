#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// JLX130-026 panel driver: 1.3" 240 x 240 IPS TFT, ST7789 controller on
// 4-wire SPI (mode 0). The module pins map to the controller as
//   CS -> chip select (active low), RST -> reset (active low),
//   RS -> data/command select (DC), SDA -> MOSI, SCK -> clock,
//   LEDA -> backlight anode.
//
// Default wiring uses idle GPIOs in the former EPD data range of the
// SVERIO Paperboard v1. Override any value with -D on the build command line.
#ifndef ST7789_PIN_CS
#define ST7789_PIN_CS 10    /* module CS */
#endif
#ifndef ST7789_PIN_RST
#define ST7789_PIN_RST 8    /* module RST */
#endif
#ifndef ST7789_PIN_DC
#define ST7789_PIN_DC 9     /* module RS */
#endif
#ifndef ST7789_PIN_MOSI
#define ST7789_PIN_MOSI 11  /* module SDA */
#endif
#ifndef ST7789_PIN_SCK
#define ST7789_PIN_SCK 12   /* module SCK */
#endif
#ifndef ST7789_PIN_BL
#define ST7789_PIN_BL 7     /* backlight enable; -1 when LEDA is tied to 3V3 */
#endif
#ifndef ST7789_SPI_CLOCK_MHZ
#define ST7789_SPI_CLOCK_MHZ 40  /* raise to 80 once the wiring is known good */
#endif
#ifndef ST7789_ROTATION
#define ST7789_ROTATION 0        /* 0..3: 0/90/180/270 degrees */
#endif

// Sets up SPI, resets the controller, runs the init sequence and enables
// the backlight. Call once, from core 0, before linux_boot().
void st7789_init(void);
void st7789_backlight(bool on);

// Draws w x h pixels at (x, y); x and y count from the top left of the
// (rotated) panel. pixels are RGB565, w * h entries, internal RAM.
esp_err_t st7789_draw_bitmap(int x, int y, int w, int h, const uint16_t* pixels);
