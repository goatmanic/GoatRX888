#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>

#include "FX3handler.h"
#include "usb_device.h"
#include "ezusb.h"
#include "firmware_usb2.h"
#include "firmware_usb3.h"

#define firmware_usb2_data ((const char *)FIRMWARE_USB2)
#define firmware_usb2_size ((uint32_t)FIRMWARE_USB2_SIZE)
#define firmware_usb3_data ((const char *)FIRMWARE_USB3)
#define firmware_usb3_size ((uint32_t)FIRMWARE_USB3_SIZE)

fx3class *CreateUsbHandler()
{
    return new fx3handler();
}

fx3handler::fx3handler()
{
    usb_device_infos = nullptr;
    dev = nullptr;
}

fx3handler::~fx3handler()
{
    Close();
}

bool fx3handler::Open()
{
    enum usb_firmware_mode mode = USB_FIRMWARE_AUTO;
    const char *request_name = "auto";

    switch (firmware_mode) {
    case Fx3FirmwareMode::Usb2:
        mode = USB_FIRMWARE_USB2;
        request_name = "usb2";
        break;
    case Fx3FirmwareMode::Usb3:
        mode = USB_FIRMWARE_USB3;
        request_name = "usb3";
        break;
    case Fx3FirmwareMode::Auto:
    default:
        mode = USB_FIRMWARE_AUTO;
        request_name = "auto";
        break;
    }

    fprintf(stderr, "FX3 firmware request: %s\n", request_name);

    dev = usb_device_open(
        devidx,
        mode,
        firmware_usb2_data,
        firmware_usb2_size,
        firmware_usb3_data,
        firmware_usb3_size
    );
    DbgPrintf("Open DevIdx=%d dev=%p\n", devidx, dev);

    // usb_device_open() returns NULL on busy / not-found / failed
    // re-enumeration. Do not touch the device in that case - Control()
    // dereferences dev and would segfault (and take down SoapySDRServer).
    if (dev == nullptr)
        return false;

    usleep(5000);
    Control(STOPFX3, (uint8_t)0);

    return true;
}

bool fx3handler::Close(void)
{
    DbgPrintf("Close dev=%p\n", dev);

    if (dev) {
        usb_device_close(dev);
        dev = nullptr;
    }

    return true;
}

bool fx3handler::IsHighSpeed()
{
    return dev != nullptr && usb_device_is_high_speed(dev) != 0;
}

bool fx3handler::Control(FX3Command command, uint8_t data)
{
    return usb_device_control(this->dev, command, 0, 0, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::Control(FX3Command command, uint32_t data)
{
    return usb_device_control(this->dev, command, 0, 0, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::Control(FX3Command command, uint64_t data)
{
    return usb_device_control(this->dev, command, 0, 0, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::SetArgument(uint16_t index, uint16_t value)
{
    uint8_t data = 0;
    return usb_device_control(this->dev, SETARGFX3, value, index, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::I2CWrite(uint8_t reg, uint16_t addr, const uint8_t *data, uint16_t len)
{
    uint8_t tmp[64];

    if (len > sizeof(tmp))
        return false;

    memcpy(tmp, data, len);

    return usb_device_control(this->dev, I2CWFX3, addr, reg, tmp, len, 0) == 0;
}

bool fx3handler::GetHardwareInfo(uint32_t *data)
{
    return usb_device_control(this->dev, TESTFX3, 0, 0, (uint8_t *)data, sizeof(*data), 1) == 0;
}

void fx3handler::StartStream(ringbuffer<int16_t> &input, int numofblock)
{
    inputbuffer = &input;
    stream = streaming_open_async(this->dev, transferSize, concurrentTransfers, PacketRead, this);
    input.setBlockSize(streaming_framesize(stream) / sizeof(int16_t));

    DbgPrintf("StartStream blocksize=%d\n", input.getBlockSize());

    // Start background thread to poll the events
    run = true;
    if (stream)
    {
        streaming_start(stream);
    }

    poll_thread = std::thread(
        [this]()
        {
            while (run)
            {
                usb_device_handle_events(this->dev);
            }
        });
}

void fx3handler::StopStream()
{
    run = false;
    poll_thread.join();

    streaming_stop(stream);
    streaming_close(stream);
}

void fx3handler::PacketRead(uint32_t data_size, uint8_t *data, void *context)
{
    fx3handler *handler = (fx3handler *)context;

    auto *ptr = handler->inputbuffer->getWritePtr();
    assert(data_size == handler->inputbuffer->getBlockSize() * sizeof(int16_t));
    memcpy(ptr, data, data_size);
    handler->inputbuffer->WriteDone();
}

bool fx3handler::ReadDebugTrace(uint8_t *pdata, uint8_t len)
{
    return true;
}

bool fx3handler::Enumerate(unsigned char &idx, char *lbuf)
{
    if (idx >= usb_device_count_devices()) return false;

    if (usb_device_infos == nullptr) {
        usb_device_get_device_list(&usb_device_infos);
    }

    auto dev = &usb_device_infos[idx];

    strcpy (lbuf, (const char*)dev->product);
    while (strlen(lbuf) < 18) strcat(lbuf, " ");
    strcat(lbuf, "sn:");
    strcat(lbuf, (const char*)dev->serial_number);
    devidx = idx;

    return true;
}