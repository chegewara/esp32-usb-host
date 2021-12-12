#include <stdio.h>
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "usb_host.hpp"
#include "usb_msc.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <esp_vfs_fat.h>
#include "wifi.h"

#define EXAMPLE_ESP_WIFI_SSID      "pendrive"
#define EXAMPLE_ESP_WIFI_PASS      ""
#define MOUNT_POINT "/files"

USBhost host;
USBmscDevice *device;

bool is_mount = false;
static void capacity_cb(usb_transfer_t *transfer)
{
    printf("capacity_cb: block_size: %d, block_count: %d\n", device->getBlockSize(0), device->getBlockCount(0));
}

static void inquiry_cb(usb_transfer_t *transfer)
{
    printf("inquiry_cb\n");
    if(!is_mount){
        device->mount(MOUNT_POINT, 0);
    }
    is_mount = true;
}

void client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        usb_device_info_t info = host.getDeviceInfo();
        ESP_LOGI("", "device speed: %s, device address: %d, max ep_ctrl size: %d, config: %d", info.speed ? "USB_SPEED_FULL" : "USB_SPEED_LOW", info.dev_addr, info.bMaxPacketSize0, info.bConfigurationValue);
        const usb_config_desc_t *config_desc = host.getConfigurationDescriptor();
        int offset = 0;

        for (size_t n = 0; n < config_desc->bNumInterfaces; n++)
        {
            const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, n, 0, &offset);

            if (intf->bInterfaceClass == 0x08) // MSC
            {
                msc_transfer_cb_t cb = {
                    .capacity_cb = capacity_cb,
                    .inquiry_cb = inquiry_cb,
                };
                device = new USBmscDevice(config_desc, &host);
                ((USBmscDevice *)device)->registerCallbacks(cb);
                n = config_desc->bNumInterfaces;
            }

            if (device)
                device->init();
        }
    } else {
        ESP_LOGI("", "DEVICE gone event");
        if(device)
        {
            device->unmount(MOUNT_POINT);
            device->deinit();
            delete(device);
            is_mount = false;
        }
        device = NULL;
    }
}

#define TAG ""
char line[5000];

static void getFreeSpace(uint64_t* used_space, uint64_t* max_space)
{
    FATFS *fs;
    DWORD dwc;
    char drv[4] = {};
    device->getDrivePath(drv);
    if (f_getfree(drv, &dwc, &fs) == FR_OK)
    {
        *used_space =
            ((uint64_t)fs->csize * (fs->n_fatent - 2 - fs->free_clst)) * fs->ssize;
        *max_space = ((uint64_t)fs->csize * (fs->n_fatent - 2)) * fs->ssize;
    }
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    esp_log_level_set("wifi", ESP_LOG_ERROR);


    // heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    host.registerClientCb(client_event_callback);
    host.init();

    initWifi(EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);

    web_server(MOUNT_POINT);

    while (!is_mount)
    {
        vTaskDelay(1);
    }

    uint64_t used_space; 
    uint64_t max_space;
    getFreeSpace(&used_space, &max_space);

    ESP_LOGI("TEST", "storage used: %lld/%lld", used_space, max_space);
}
