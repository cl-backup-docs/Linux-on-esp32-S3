// Shared ESP_KBD_IF sink for the keyboard front-ends: the USB host driver
// (usb_kbd.c) and the BLE HOGP client (ble_kbd.c). Every typed byte leaves as
// one message to the Linux side, where kernel-driver-esp32-ng/esp_kbd.c feeds
// it to ttyS0 and /dev/pbkbd.
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "interface.h"
#include "adapter.h"
#include "pb_kbd_bridge.h"

extern QueueHandle_t to_host_queue[MAX_PRIORITY_QUEUES];
void pb_console_write(const uint8_t*, size_t);

// One ESP_KBD_IF message for a burst of typed bytes; mirrors ble_prov_to_linux.
int pb_kbd_send(const uint8_t* bytes, size_t len) {
    interface_buffer_handle_t buf_handle;
    uint8_t* buf;
    if (!to_host_queue[PRIO_Q_MID])
        return -1;
    buf = malloc(len);
    if (!buf)
        return -1;
    memcpy(buf, bytes, len);
    memset(&buf_handle, 0, sizeof(buf_handle));
    buf_handle.if_type = ESP_KBD_IF;
    buf_handle.if_num = 0;
    buf_handle.payload_len = len;
    buf_handle.payload = buf;
    buf_handle.wlan_buf_handle = buf;
    buf_handle.free_buf_handle = free;
    if (xQueueSend(to_host_queue[PRIO_Q_MID], &buf_handle, pdMS_TO_TICKS(20)) != pdTRUE) {
        free(buf);
        return -1;
    }
    return 0;
}

// Render a status line on the panel console, so a keyboard-only user can see
// what the BLE front-end is doing (scan, passkey, ready).
void pb_kbd_notice(const char* text) {
    pb_console_write((const uint8_t*)text, strlen(text));
}
