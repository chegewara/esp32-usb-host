#pragma once
#include "usb/usb_host.h"
#include "usb_host.hpp"

// class USBhost;
class USBhostDevice
{
protected:
    USBhost* _host;
    uint8_t itf_num;
    
public:
    USBhostDevice();
    ~USBhostDevice();

    virtual bool init() = 0;

};

