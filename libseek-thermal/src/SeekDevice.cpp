/*
 *  Seek device interface
 *  Author: Maarten Vandersteegen
 */

#include "SeekDevice.h"
#include "SeekLogging.h"
#include <libusb.h>
#include <endian.h>
#include <stdio.h>

using namespace LibSeek;

class libusb_adapter
{
private:
    static libusb_adapter* instance;

    libusb_adapter()
    {
        int res;
        res = libusb_init(&m_ctx);
        if (res < 0)
        {
            error("Error: libusb init failed: %s\n", libusb_error_name(res));
        }
        
    }

    libusb_adapter(const libusb_adapter&) {}

    libusb_context* m_ctx;

public:
    ~libusb_adapter()
    {
        if (m_ctx != NULL) {
            libusb_exit(m_ctx);
            m_ctx = NULL;
        }
        instance = nullptr;
    }

    static libusb_adapter* getInstance()
    {
        if (instance == nullptr)
        {
            instance = new libusb_adapter();
        }
        return instance;
    }

    libusb_context* getContext()
    {
        return m_ctx;
    }
};
libusb_adapter* libusb_adapter::instance = nullptr;

SeekDevice::SeekDevice(int vendor_id, int product_id, int id , int timeout) :
    m_vendor_id(vendor_id),
    m_product_id(product_id),
    m_timeout(timeout),
    m_is_opened(false),
    //m_ctx(nullptr),
    m_handle(nullptr),
    m_index(id)
    { }

SeekDevice::~SeekDevice()
{
    close();
};

bool SeekDevice::open()
{
    int res;
    int bConfigurationValue;

    if (m_handle != NULL) {
        error("Error: SeekDevice already opened\n");
        return false;
    }

    //libusb_set_debug(NULL, LIBUSB_LOG_LEVEL_WARNING);

    // Init libusb
    /*res = libusb_init(&m_ctx);
    if (res < 0) {
        error("Error: libusb init failed: %s\n", libusb_error_name(res));
        return false;
    }*/


    if (!open_device()) {
        close();
        return false;
    }

    res = libusb_get_configuration(m_handle, &bConfigurationValue);
    if (res != 0) {
        error("Error: libusb get configuration failed: %s\n", libusb_error_name(res));
        close();
        return false;
    }
    debug("bConfigurationValue : %d\n", bConfigurationValue);

    if (bConfigurationValue != 1) {
        res = libusb_set_configuration(m_handle, 1);
        if (res != 0) {
            error("Error: libusb set configuration failed: %s\n", libusb_error_name(res));
            close();
            return false;
        }
    }

    res = libusb_claim_interface(m_handle, 0);
    if (res < 0) {
        error("Error: failed to claim interface: %s\n", libusb_error_name(res));
        close();
        return false;
    }

    m_is_opened = true;
    return true;
}

void SeekDevice::close()
{
    if (m_handle != NULL) {
        libusb_release_interface(m_handle, 0);  /* release claim */
        libusb_close(m_handle);                 /* revert open */
        m_handle = NULL;
    }

    /*if (m_ctx != NULL) {
        libusb_exit(m_ctx);                     // revert exit 
        m_ctx = NULL;
    }*/

    m_is_opened = false;
}

bool SeekDevice::isOpened()
{
    return m_is_opened;
}

bool SeekDevice::request_set(DeviceCommand::Enum command, std::vector<uint8_t>& data)
{
    return control_transfer(0, static_cast<char>(command), 0, 0, data);
}

bool SeekDevice::request_get(DeviceCommand::Enum command, std::vector<uint8_t>& data)
{
    return control_transfer(1, static_cast<char>(command), 0, 0, data);
}

bool SeekDevice::fetch_frame(uint16_t* buffer, std::size_t size)
{
    int res;
    int actual_length;
    int todo = size * sizeof(uint16_t);
    uint8_t* buf = reinterpret_cast<uint8_t*>(buffer);
    int done = 0;

    while (todo != 0) {
        debug("Asking for %d B of data at %d\n", todo, done);
        res = libusb_bulk_transfer(m_handle, 0x81, &buf[done], todo, &actual_length, m_timeout);
        if (res != 0) {
            error("Error: bulk transfer failed: %s\n", libusb_error_name(res));
            return false;
        }
        debug("Actual length %d\n", actual_length);
        todo -= actual_length;
        done += actual_length;
    }
    correct_endianness(buffer, size);

    return true;
}

bool SeekDevice::open_device()
{
    int res;
    int idx_dev;
    int cnt;
    int found = -1;
    struct libusb_device **devs;
    struct libusb_device_descriptor desc;

    cnt = libusb_get_device_list(libusb_adapter::getInstance()->getContext(), &devs);
    if (cnt < 0) {
        error("Error: no devices found: %s\n", libusb_error_name(cnt));
        return false;
    }

    debug("Device Count : %d\n", cnt);

    for (idx_dev = 0; idx_dev < cnt; idx_dev++) {
        res = libusb_get_device_descriptor(devs[idx_dev], &desc);
        if (res < 0) {
            libusb_free_device_list(devs, 1);
            error("Error: failed to get device descriptor: %s\n", libusb_error_name(res));
            return false;
        }

        debug("vendor: %x  product: %x\n", desc.idVendor, desc.idProduct);

        if (desc.idVendor == m_vendor_id && desc.idProduct == m_product_id) {
            found++;
            if (m_index == found)
            {
                break;
            }
        }
    }

    if (found < 0) {
        libusb_free_device_list(devs, 1);
        error("Error: Did not found device %04x:%04x\n", m_vendor_id, m_product_id);
        return false;
    }

    res = libusb_open(devs[idx_dev], &m_handle);
    libusb_free_device_list(devs, 1);

    if (res < 0) {
        error("Error: libusb init failed: %s\n", libusb_error_name(res));
        return false;
    }

    return true;
}


bool SeekDevice::control_transfer(bool direction, uint8_t req, uint16_t value, uint16_t index, std::vector<uint8_t>& data)
{
    int res;
    uint8_t bmRequestType = (direction ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT)
                            | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE;
    if (data.size() == 0) {
        data.reserve(16);
    }
    
    uint8_t* aData = data.data();
    uint16_t wLength = data.size();

    // to device
    debug("ctrl_transfer to/from dev(0x%x, 0x%x, 0x%x, 0x%x, %d)\n",
                    bmRequestType, req, value, index, wLength);

    res = libusb_control_transfer(m_handle, bmRequestType, req, value, index, aData, wLength, m_timeout);

    if (res < 0) {
        error("Error: control transfer failed: %s\n", libusb_error_name(res));
        return false;
    }

    if (res != wLength) {
        error("Error: control transfer returned %d bytes, expected %d bytes\n", res, wLength);
        return false;
    }

    return true;
}

void SeekDevice::correct_endianness(uint16_t* buffer, std::size_t size)
{
    std::size_t i;

    for (i=0; i<size; i++) {
        buffer[i] = le16toh(buffer[i]);
    }
}
