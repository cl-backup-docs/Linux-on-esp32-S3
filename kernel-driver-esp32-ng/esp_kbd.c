// SPDX-License-Identifier: GPL-2.0-only
/*
 * Paperboard USB keyboard bridge.
 *
 * The ESP32 firmware owns the USB-OTG host controller and forwards boot
 * keyboard reports as ESP_KBD_IF payloads.  A keyboard has no wire to
 * inject into, so the bytes are queued into the ttyS0 input stream: the
 * login prompt, the shell and anything else reading the serial console
 * see them exactly as if they had been typed on the serial line.
 *
 * The same stream is mirrored through /dev/pbkbd.  Reads observe what the
 * keyboard sent; writes inject bytes directly into ttyS0.
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/esp32_uart.h>

#include "esp.h"
#include "utils.h"
#include "esp_api.h"

#define ESP_KBD_DEV_NAME	"pbkbd"
#define ESP_KBD_TTY_LINE	0	/* ttyS0 is the console */
#define ESP_KBD_MAX_WRITE	256
#define ESP_KBD_FIFO_SIZE	512

static struct esp_adapter *kbd_adapter;
static bool kbd_registered;
static DEFINE_MUTEX(kbd_lock);
static DECLARE_KFIFO(kbd_fifo, u8, ESP_KBD_FIFO_SIZE);
static wait_queue_head_t kbd_wait;
static atomic_t kbd_open = ATOMIC_INIT(0);

static void esp_kbd_inject(const u8 *data, size_t len)
{
	int ret;

	ret = esp32_uart_inject_input(ESP_KBD_TTY_LINE, data, len);
	if (ret <= 0)
		return;

	kfifo_in(&kbd_fifo, data, ret);
	wake_up_interruptible(&kbd_wait);
}

void esp_kbd_rx(struct sk_buff *skb)
{
	if (!skb)
		return;

	mutex_lock(&kbd_lock);
	if (kbd_adapter && skb->len)
		esp_kbd_inject(skb->data, skb->len);
	mutex_unlock(&kbd_lock);

	dev_kfree_skb_any(skb);
}

static int esp_kbd_open(struct inode *inode, struct file *file)
{
	if (atomic_xchg(&kbd_open, 1))
		return -EBUSY;

	mutex_lock(&kbd_lock);
	kfifo_reset(&kbd_fifo);
	mutex_unlock(&kbd_lock);

	return 0;
}

static int esp_kbd_release(struct inode *inode, struct file *file)
{
	atomic_set(&kbd_open, 0);
	return 0;
}

static ssize_t esp_kbd_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	unsigned int copied;
	int ret;

	if (kfifo_is_empty(&kbd_fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(kbd_wait,
					       !kfifo_is_empty(&kbd_fifo));
		if (ret)
			return ret;
	}

	ret = kfifo_to_user(&kbd_fifo, buf, count, &copied);

	return ret ? ret : copied;
}

static ssize_t esp_kbd_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	u8 data[ESP_KBD_MAX_WRITE];

	if (!count)
		return 0;
	count = min_t(size_t, count, sizeof(data));
	if (copy_from_user(data, buf, count))
		return -EFAULT;

	return esp32_uart_inject_input(ESP_KBD_TTY_LINE, data, count);
}

static __poll_t esp_kbd_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &kbd_wait, wait);
	if (!kfifo_is_empty(&kbd_fifo))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static const struct file_operations esp_kbd_fops = {
	.owner		= THIS_MODULE,
	.open		= esp_kbd_open,
	.release	= esp_kbd_release,
	.read		= esp_kbd_read,
	.write		= esp_kbd_write,
	.poll		= esp_kbd_poll,
	.llseek		= no_llseek,
};

static struct miscdevice esp_kbd_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= ESP_KBD_DEV_NAME,
	.fops	= &esp_kbd_fops,
	.mode	= 0600,
};

int esp_kbd_init(struct esp_adapter *adapter)
{
	int ret;

	init_waitqueue_head(&kbd_wait);

	ret = misc_register(&esp_kbd_misc);
	if (ret) {
		esp_err("Failed to register /dev/%s: %d\n",
			ESP_KBD_DEV_NAME, ret);
		return ret;
	}

	mutex_lock(&kbd_lock);
	kbd_adapter = adapter;
	kbd_registered = true;
	mutex_unlock(&kbd_lock);

	esp_info("USB keyboard bridge at /dev/%s\n", ESP_KBD_DEV_NAME);
	return 0;
}

void esp_kbd_deinit(void)
{
	mutex_lock(&kbd_lock);
	if (kbd_registered) {
		misc_deregister(&esp_kbd_misc);
		kbd_registered = false;
	}
	kbd_adapter = NULL;
	mutex_unlock(&kbd_lock);
}
