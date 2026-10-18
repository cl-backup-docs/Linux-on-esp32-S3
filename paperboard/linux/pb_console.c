// ESP-IDF side of the Paperboard text console. All renderer work stays on core 0.
// Linux is unchanged: epd-shell writes /dev/epd, the kernel forwards the bytes
// over shared memory, and pb_console_write() renders them onto the ST7789 panel.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "st7789.h"
#include "terminal.h"
static PbTerminal current,shown;
static bool dirty[PB_ROWS];
static QueueHandle_t queue;
static bool full_paint=true;
// One text row of RGB565 pixels; st7789_draw_bitmap() byte-swaps into its own
// scratch buffer, so this stays plain RGB565 in internal RAM.
static uint16_t strip[PB_CELL_H*PB_WIDTH] __attribute__((aligned(4)));
struct chunk { unsigned len; uint8_t bytes[128]; };
// The panel takes a new frame in milliseconds; redraw shortly after activity
// instead of at the old e-paper pace.
#define PB_REFRESH_MS 100
static void console_task(void* arg) {
    (void)arg;
    TickType_t last=xTaskGetTickCount();
    bool pending=true,failed=false;
    for(;;) {
        struct chunk c;
        if(xQueueReceive(queue,&c,pdMS_TO_TICKS(20))==pdTRUE) {
            pb_feed(&current,c.bytes,c.len);pending=true;
        }
        if(!pending || failed || xTaskGetTickCount()-last<pdMS_TO_TICKS(PB_REFRESH_MS))continue;
        if(full_paint) {memset(dirty,true,sizeof(dirty));full_paint=false;}
        else pb_dirty_rows(&current,&shown,dirty);
        for(unsigned row=0;row<PB_ROWS&&!failed;row++) {
            if(!dirty[row])continue;
            pb_render_cell_row(&current,row,strip);
            esp_err_t err=st7789_draw_bitmap(0,row*PB_CELL_H,PB_WIDTH,PB_CELL_H,strip);
            if(err!=ESP_OK) {
                ESP_LOGE("paperboard","draw failed 0x%x; display stopped until reboot",err);
                failed=true;
            }
        }
        if(!failed)shown=current;
        last=xTaskGetTickCount();pending=false;
    }
}
void pb_console_write(const uint8_t* data,size_t len) {
    if(!queue)return;
    while(len) {
        struct chunk c={.len=len>128?128:len};
        memcpy(c.bytes,data,c.len);
        xQueueSend(queue,&c,portMAX_DELAY);
        data+=c.len;len-=c.len;
    }
}
void pb_console_init(void) {
    // Must run on core 0 BEFORE linux_boot takes over core 1. No external-RAM malloc.
    configASSERT(xPortGetCoreID()==0);
    st7789_init();
    printf("paperboard v4: core=%d panel=JLX130-026/ST7789 grid=%dx%d spi=%dMHz bl_gpio=%d internal_free=%u\n",
           xPortGetCoreID(),PB_COLS,PB_ROWS,ST7789_SPI_CLOCK_MHZ,ST7789_PIN_BL,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    pb_init(&current);pb_init(&shown);shown.cursor_visible=false;
    const char* banner="Paperboard / JLX130-026 (ST7789 240x240)\r\nLinux console: run epd-shell over serial.\r\n\r\n";
    pb_feed(&current,(const uint8_t*)banner,strlen(banner));
    queue=xQueueCreate(8,sizeof(struct chunk));configASSERT(queue);
    configASSERT(xTaskCreatePinnedToCore(console_task,"paperboard",6144,NULL,2,NULL,0)==pdPASS);
}
