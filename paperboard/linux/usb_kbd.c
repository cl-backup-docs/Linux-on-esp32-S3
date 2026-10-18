// ESP-IDF side of the Paperboard USB keyboard bridge. A USB HID boot keyboard
// plugged into the native USB port (GPIO19/20) is enumerated as a host, and
// every report is converted (common/hid_keymap.c) into the byte stream a
// serial terminal would send. The bytes leave as ESP_KBD_IF messages; the
// Linux driver (kernel-driver-esp32-ng/esp_kbd.c) feeds them to ttyS0 and
// /dev/pbkbd, so login and mirrored sessions work with no serial link.
//
// Everything here stays on core 0; Linux owns core 1. Taking the USB PHY for
// host mode disconnects the USB-Serial/JTAG console while the firmware runs
// (the sdkconfig profile disables that secondary console), so IDF logs go to
// UART0 only and flashing through the native port needs BOOT+RESET.
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "interface.h"
#include "adapter.h"
#include "hid_keymap.h"

extern QueueHandle_t to_host_queue[MAX_PRIORITY_QUEUES];
void pb_console_write(const uint8_t*, size_t);

static const char* TAG = "usb_kbd";

#define KBD_CLASS_REQUEST_SET_IDLE 0x0a
#define KBD_CLASS_REQUEST_SET_PROTOCOL 0x0b
#define KBD_TASK_PRIO 5
#define KBD_LIB_STACK 4096
#define KBD_TASK_STACK 5120
#define KBD_MAX_PENDING 64
#define MOUSE_PROTOCOL 2

static usb_host_client_handle_t kbd_client;
static usb_device_handle_t kbd_dev;      // opened keyboard, NULL when absent
static usb_transfer_t* kbd_xfer;         // interrupt IN transfer (when active)
static bool kbd_xfer_inflight;
static uint8_t kbd_ep;                   // interrupt IN endpoint address
static uint8_t kbd_if_num;               // claimed interface number
static PbKeyboard kbd;
static uint8_t pending[KBD_MAX_PENDING];
static size_t pending_len;
static volatile bool new_dev_pending;
static volatile uint8_t new_dev_addr;
static volatile bool dev_gone_pending;
static volatile bool ctrl_done;
static volatile usb_transfer_status_t ctrl_status;

static void kbd_notice(const char* text) {
    pb_console_write((const uint8_t*)text, strlen(text));
}

// One ESP_KBD_IF message for a burst of typed bytes; mirrors ble_prov_to_linux.
static int kbd_send(const uint8_t* bytes, size_t len) {
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
static void kbd_flush(void) {
    if (!pending_len)
        return;
    if (kbd_send(pending, pending_len))
        ESP_LOGW(TAG, "dropped %u keyboard bytes, host link busy", (unsigned)pending_len);
    pending_len = 0;
}
static void kbd_emit(void* ctx, const uint8_t* bytes, size_t len) {
    (void)ctx;
    while (len) {
        if (pending_len == sizeof(pending))
            kbd_flush();
        pending[pending_len++] = *bytes++;
        len--;
    }
}

static void ctrl_cb(usb_transfer_t* transfer) {
    ctrl_status = transfer->status;
    ctrl_done = true;
}
// Class request without a data stage, e.g. SET_PROTOCOL(0). The client event
// loop is pumped while waiting, so this must be called from the keyboard task,
// never from inside one of the USB callbacks.
static bool kbd_control(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index) {
    usb_transfer_t* xfer = NULL;
    usb_setup_packet_t* setup;
    int64_t deadline;
    bool ok = false;
    if (!kbd_dev || usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE, 0, &xfer) != ESP_OK)
        return false;
    xfer->device_handle = kbd_dev;
    xfer->bEndpointAddress = 0;
    xfer->callback = ctrl_cb;
    xfer->context = NULL;
    xfer->num_bytes = USB_SETUP_PACKET_SIZE;
    setup = (usb_setup_packet_t*)xfer->data_buffer;
    setup->bmRequestType = req_type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = 0;
    ctrl_done = false;
    if (usb_host_transfer_submit_control(kbd_client, xfer) != ESP_OK) {
        usb_host_transfer_free(xfer);
        ESP_LOGW(TAG, "control request %u not submitted", request);
        return false;
    }
    deadline = esp_timer_get_time() / 1000 + 2000;
    while (!ctrl_done && esp_timer_get_time() / 1000 < deadline)
        usb_host_client_handle_events(kbd_client, pdMS_TO_TICKS(10));
    if (!ctrl_done) {
        ESP_LOGE(TAG, "control request %u stuck; leaking one transfer", request);
        return false;
    }
    ok = ctrl_status == USB_TRANSFER_STATUS_COMPLETED;
    usb_host_transfer_free(xfer);
    return ok;
}

static void kbd_submit(void) {
    if (!kbd_dev || !kbd_xfer)
        return;
    kbd_xfer->num_bytes = kbd_xfer->data_buffer_size;
    if (usb_host_transfer_submit(kbd_xfer) == ESP_OK)
        kbd_xfer_inflight = true;
}
static void kbd_xfer_cb(usb_transfer_t* transfer) {
    kbd_xfer_inflight = false;
    if (!kbd_dev)                       // removed while completing
        return;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (transfer->actual_num_bytes >= PB_HID_REPORT_LEN) {
            pb_kbd_report(&kbd, transfer->data_buffer,
                          esp_timer_get_time() / 1000, kbd_emit, NULL);
            kbd_flush();
        }
        kbd_submit();
    } else if (transfer->status != USB_TRANSFER_STATUS_CANCELED &&
               transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "interrupt IN transfer failed (%d); clearing endpoint",
                 transfer->status);
        usb_host_endpoint_clear(kbd_dev, kbd_ep);
        kbd_submit();
    }
}

static void teardown_device(void) {
    usb_device_handle_t dev = kbd_dev;
    if (!dev)
        return;
    kbd_dev = NULL;                     // stops the transfer callback resubmitting
    for (int i = 0; kbd_xfer_inflight && i < 200; i++)
        usb_host_client_handle_events(kbd_client, pdMS_TO_TICKS(10));
    if (kbd_xfer_inflight)
        ESP_LOGE(TAG, "transfer stuck after disconnect; leaking one transfer");
    else
        usb_host_transfer_free(kbd_xfer);
    kbd_xfer = NULL;
    usb_host_interface_release(kbd_client, dev, kbd_if_num);
    usb_host_device_close(kbd_client, dev);
    pending_len = 0;
    pb_kbd_init(&kbd);
    ESP_LOGI(TAG, "USB keyboard removed");
    kbd_notice("\r\n[usb keyboard removed]\r\n");
}

// Opens a freshly enumerated device and starts the report pump if it exposes
// the standard HID keyboard interface (class 3, subclass 1, protocol 0 or 1;
// SET_PROTOCOL below switches it to the boot layout either way).
static bool probe_device(uint8_t addr) {
    usb_device_handle_t dev = NULL;
    const usb_device_desc_t* dd = NULL;
    const usb_config_desc_t* cd = NULL;
    const usb_standard_desc_t* desc;
    const usb_intf_desc_t* intf = NULL;
    const usb_ep_desc_t* ep = NULL;
    int offset = 0;
    size_t mps;

    if (kbd_dev)
        return false;
    if (usb_host_device_open(kbd_client, addr, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open USB device at address %u", addr);
        return false;
    }
    usb_host_get_device_descriptor(dev, &dd);
    usb_host_get_active_config_descriptor(dev, &cd);
    desc = (const usb_standard_desc_t*)cd;
    while (desc) {                      // find the boot keyboard interface
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* it = (const usb_intf_desc_t*)desc;
            if (it->bNumEndpoints && it->bInterfaceClass == USB_CLASS_HID &&
                it->bInterfaceSubClass == 1 && it->bInterfaceProtocol <= 1) {
                intf = it;
                break;
            }
            if (it->bInterfaceClass == USB_CLASS_HID &&
                it->bInterfaceSubClass == 1 && it->bInterfaceProtocol == MOUSE_PROTOCOL)
                ESP_LOGD(TAG, "ignoring HID mouse at address %u", addr);
        }
        desc = usb_parse_next_descriptor(desc, cd->wTotalLength, &offset);
    }
    if (intf) {                         // its first interrupt IN endpoint
        const usb_standard_desc_t* d = (const usb_standard_desc_t*)intf;
        int off = offset;
        while ((d = usb_parse_next_descriptor(d, cd->wTotalLength, &off)) != NULL) {
            if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE)
                break;
            if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
                const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
                if ((e->bmAttributes & 0x03) == 0x03 && (e->bEndpointAddress & 0x80)) {
                    ep = e;
                    break;
                }
            }
        }
    }
    if (!intf || !ep) {
        if (dd)
            ESP_LOGI(TAG, "USB device %04x:%04x is not a boot keyboard",
                     (unsigned)dd->idVendor, (unsigned)dd->idProduct);
        usb_host_device_close(kbd_client, dev);
        return false;
    }
    if (usb_host_interface_claim(kbd_client, dev, intf->bInterfaceNumber,
                                 intf->bAlternateSetting) != ESP_OK) {
        ESP_LOGW(TAG, "cannot claim HID interface %u", intf->bInterfaceNumber);
        usb_host_device_close(kbd_client, dev);
        return false;
    }
    kbd_dev = dev;
    if (!kbd_control(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                         USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                     KBD_CLASS_REQUEST_SET_PROTOCOL, 0, intf->bInterfaceNumber))
        ESP_LOGW(TAG, "SET_PROTOCOL(boot) failed; report layout may differ");
    kbd_control(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                    USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                KBD_CLASS_REQUEST_SET_IDLE, 0, intf->bInterfaceNumber);
    kbd_if_num = intf->bInterfaceNumber;
    kbd_ep = ep->bEndpointAddress;
    mps = (size_t)(ep->wMaxPacketSize & 0x07ff);
    if (mps < PB_HID_REPORT_LEN)
        mps = PB_HID_REPORT_LEN;
    if (usb_host_transfer_alloc(mps, 0, &kbd_xfer) != ESP_OK) {
        ESP_LOGE(TAG, "cannot allocate interrupt IN transfer");
        kbd_dev = NULL;
        usb_host_interface_release(kbd_client, dev, kbd_if_num);
        usb_host_device_close(kbd_client, dev);
        return false;
    }
    kbd_xfer->device_handle = kbd_dev;
    kbd_xfer->bEndpointAddress = kbd_ep;
    kbd_xfer->callback = kbd_xfer_cb;
    kbd_xfer->context = NULL;
    pb_kbd_init(&kbd);
    pending_len = 0;
    kbd_submit();
    ESP_LOGI(TAG, "USB keyboard ready (device %04x:%04x, endpoint 0x%02x, mps %u)",
             (unsigned)dd->idVendor, (unsigned)dd->idProduct, kbd_ep, (unsigned)mps);
    kbd_notice("\r\n[usb keyboard ready]\r\n");
    return true;
}

static void client_event_cb(const usb_host_client_event_msg_t* msg, void* arg) {
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (!kbd_dev) {
            new_dev_addr = msg->new_dev.address;
            new_dev_pending = true;
        }
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (kbd_dev && msg->dev_gone.dev_hdl == kbd_dev)
            dev_gone_pending = true;
    }
}

static void usb_lib_task(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            ESP_LOGD(TAG, "usb host library: no clients");
    }
}

static void usb_kbd_task(void* arg) {
    (void)arg;
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
    };
    if (usb_host_client_register(&client_config, &kbd_client) != ESP_OK) {
        ESP_LOGE(TAG, "cannot register USB host client; keyboard disabled");
        vTaskDelete(NULL);
        return;
    }
    {   // keyboards attached before this task ran produce no NEW_DEV event
        uint8_t addrs[8];
        int n = 0;
        if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK)
            for (int i = 0; i < n && !kbd_dev; i++)
                probe_device(addrs[i]);
    }
    for (;;) {
        usb_host_client_handle_events(kbd_client, pdMS_TO_TICKS(20));
        if (new_dev_pending) {
            new_dev_pending = false;
            if (!kbd_dev)
                probe_device(new_dev_addr);
        }
        if (dev_gone_pending) {
            dev_gone_pending = false;
            teardown_device();
        }
        pb_kbd_tick(&kbd, esp_timer_get_time() / 1000, kbd_emit, NULL);
        kbd_flush();
    }
}

void usb_kbd_init(void) {
    usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s; keyboard disabled",
                 esp_err_to_name(err));
        return;
    }
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", KBD_LIB_STACK, NULL,
                                KBD_TASK_PRIO, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(usb_kbd_task, "usb_kbd", KBD_TASK_STACK, NULL,
                                KBD_TASK_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "cannot start USB tasks; keyboard disabled");
        return;
    }
    ESP_LOGI(TAG, "USB host running; plug a boot-protocol keyboard into the USB port");
}
