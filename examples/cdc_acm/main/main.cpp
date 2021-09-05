#include <stdio.h>
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#include "usb_host.hpp"
#include "usb_acm.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <esp_vfs_fat.h>

USBhost host;
USBacmDevice *device;

bool is_ready = false;

void callback(int event, void *data, size_t len)
{
    switch (event)
    {
    case CDC_CTRL_SET_CONTROL_LINE_STATE:
        printf("CDC_CTRL_SET_CONTROL_LINE_STATE\n");
        device->setLineCoding(115200, 0, 0, 8);
        break;

    case CDC_DATA_IN:
    {
        device->INDATA();
        char *buf = (char *)data;
        buf[len] = 0;
        printf("%s", (char *)data);
        break;
    }
    case CDC_DATA_OUT:

        break;

    case CDC_CTRL_SET_LINE_CODING:
        printf("CDC_CTRL_SET_LINE_CODING\n");
        break;
    }
}

void client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        host.open(event_msg);
        usb_device_info_t info = host.getDeviceInfo();
        ESP_LOGI("", "device speed: %s, device address: %d, max ep_ctrl size: %d, config: %d", info.speed ? "USB_SPEED_FULL" : "USB_SPEED_LOW", info.dev_addr, info.bMaxPacketSize0, info.bConfigurationValue);
        const usb_config_desc_t *config_desc = host.getConfigurationDescriptor();
        int offset = 0;

        for (size_t n = 0; n < config_desc->bNumInterfaces; n++)
        {
            const usb_intf_desc_t *intf = usb_host_parse_interface(config_desc, n, 0, &offset);

            if (intf->bInterfaceClass == 0x0a) // CDC ACM
            {
                device = new USBacmDevice(config_desc, &host);
                n = config_desc->bNumInterfaces;
            }

            if (device)
            {
                device->init();
                device->onEvent(callback);
                device->setControlLine(1, 1);
                device->INDATA();
            }
        }
    }
    else
    {
        ESP_LOGW("", "DEVICE gone event");
    }
}

extern "C" void app_main(void)
{
    // heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    host.registerClientCb(client_event_callback);
    host.init();

    while (!device)
    {
        vTaskDelay(10);
    }

    while (1)
    {
        while (!device->isConnected())
        {
            vTaskDelay(10);
        }

        vTaskDelay(100);
        device->OUTDATA((uint8_t *)"test\n", 5);
    }
}
