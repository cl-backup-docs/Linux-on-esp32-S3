// ST7789 driver for the JLX130-026 (240 x 240 IPS) panel. Single caller:
// the pb_console renderer on core 0. Polling transfers are intentional here.
#include "st7789.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ST7789_HOST SPI2_HOST
#define ST7789_W 240
#define ST7789_H 240

// MADCTL for rotations 0/90/180/270 (MX = 0x40, MY = 0x80, MV = 0x20).
static const uint8_t madctl[4] = {0x00, 0x60, 0xc0, 0xa0};

static spi_device_handle_t spi;
static uint8_t cmd_params[8] __attribute__((aligned(4)));  // parameter scratch, internal RAM

static void tx(const void* data, size_t len) {
    spi_transaction_t t = {0};
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    } else {
        t.tx_buffer = data;
    }
    t.length = len * 8;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
}

static void cmd(uint8_t c) {
    gpio_set_level(ST7789_PIN_DC, 0);
    tx(&c, 1);
}

static void cmd_data(uint8_t c, const uint8_t* data, size_t len) {
    cmd(c);
    gpio_set_level(ST7789_PIN_DC, 1);
    while (len) {
        size_t take = len > sizeof(cmd_params) ? sizeof(cmd_params) : len;
        memcpy(cmd_params, data, take);
        tx(cmd_params, take);
        data += take;
        len -= take;
    }
}

void st7789_backlight(bool on) {
#if ST7789_PIN_BL >= 0
    gpio_set_level(ST7789_PIN_BL, on ? 1 : 0);
#else
    (void)on;
#endif
}

void st7789_init(void) {
    _Static_assert(ST7789_ROTATION >= 0 && ST7789_ROTATION <= 3, "ST7789_ROTATION must be 0..3");

    gpio_config_t ctrl = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << ST7789_PIN_DC) | (1ULL << ST7789_PIN_RST),
    };
#if ST7789_PIN_BL >= 0
    ctrl.pin_bit_mask |= 1ULL << ST7789_PIN_BL;
#endif
    ESP_ERROR_CHECK(gpio_config(&ctrl));
    st7789_backlight(false);

    spi_bus_config_t bus = {
        .mosi_io_num = ST7789_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = ST7789_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ST7789_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = ST7789_SPI_CLOCK_MHZ * 1000 * 1000,
        .mode = 0,
        .spics_io_num = ST7789_PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(ST7789_HOST, &dev, &spi));

    gpio_set_level(ST7789_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(ST7789_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    cmd(0x01);  // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11);  // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    {
        uint8_t v = 0x55;  // COLMOD: 16-bit RGB565
        cmd_data(0x3a, &v, 1);
    }
    {
        uint8_t v = madctl[ST7789_ROTATION];  // MADCTL
        cmd_data(0x36, &v, 1);
    }
    {
        uint8_t v[5] = {0x0c, 0x0c, 0x00, 0x33, 0x33};  // PORCTRL
        cmd_data(0xb2, v, sizeof(v));
    }
    {
        uint8_t v = 0x35;  // GATECTRL
        cmd_data(0xb7, &v, 1);
    }
    {
        uint8_t v = 0x35;  // VCOM
        cmd_data(0xbb, &v, 1);
    }
    {
        uint8_t v = 0x2c;  // LCMCTRL
        cmd_data(0xc0, &v, 1);
    }
    {
        uint8_t v[2] = {0x01, 0xff};  // VDV/VRH enable
        cmd_data(0xc2, v, sizeof(v));
    }
    {
        uint8_t v = 0x12;  // VRHS
        cmd_data(0xc3, &v, 1);
    }
    {
        uint8_t v = 0x20;  // VDVS
        cmd_data(0xc4, &v, 1);
    }
    {
        uint8_t v = 0x0f;  // FRCTRL2
        cmd_data(0xc6, &v, 1);
    }
    {
        uint8_t v[2] = {0xa4, 0xa1};  // PWCTRL1
        cmd_data(0xd0, v, sizeof(v));
    }
    cmd(0x21);  // INVON: the IPS panel needs display inversion
    cmd(0x13);  // NORON
    cmd(0x29);  // DISPON
    vTaskDelay(pdMS_TO_TICKS(50));

    st7789_backlight(true);
}

esp_err_t st7789_draw_bitmap(int x, int y, int w, int h, const uint16_t* pixels) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > ST7789_W || y + h > ST7789_H)
        return ESP_ERR_INVALID_ARG;
    uint8_t caset[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
    uint8_t raset[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1)};
    cmd_data(0x2a, caset, sizeof(caset));  // CASET
    cmd_data(0x2b, raset, sizeof(raset));  // RASET
    cmd(0x2c);                             // RAMWR

    // The controller wants the high byte of every RGB565 pixel first, so
    // transmit through a small swapping buffer.
    static uint16_t swap[256] __attribute__((aligned(4)));
    size_t left = (size_t)w * h;
    gpio_set_level(ST7789_PIN_DC, 1);
    while (left) {
        size_t take = left > 256 ? 256 : left;
        for (size_t i = 0; i < take; i++) swap[i] = (uint16_t)((pixels[i] >> 8) | (pixels[i] << 8));
        spi_transaction_t t = {.length = take * 16, .tx_buffer = swap};
        esp_err_t err = spi_device_polling_transmit(spi, &t);
        if (err != ESP_OK) return err;
        pixels += take;
        left -= take;
    }
    return ESP_OK;
}
