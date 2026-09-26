// USB device/configuration/string descriptors for this project's own
// demo firmware image - deliberately *not* part of the estrp2040 module
// itself (Serial's own doc comment, estrp2040/src/serial.cppm, has the
// full "product identity vs. mechanism" reasoning): VID/PID and product
// strings are this specific firmware's own concern, not something a
// reusable platform backend should hardcode. A separate, future project
// building against this framework supplies its own version of this file.
//
// VID/PID below (0xCafe/0x4001) are TinyUSB's own well-known example
// values, not a real assigned identity - fine for this demo, not for a
// shipped product.
#include <string.h>

#include "tusb.h"

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCafe,
    .idProduct = 0x4001,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t* tud_descriptor_device_cb(void) {
  return (const uint8_t*)&device_descriptor;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

const uint8_t* tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return configuration_descriptor;
}

static const char* string_descriptors[] = {
    NULL, // index 0 is the language ID, handled specially below
    "est",
    "estrp2040 USB CDC demo",
    "000000",
    "est CDC",
};

static uint16_t string_descriptor_buf[32];

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t chr_count;

  if (index == 0) {
    string_descriptor_buf[1] = 0x0409; // English (United States)
    chr_count = 1;
  } else {
    if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
      return NULL;
    }
    const char* str = string_descriptors[index];
    chr_count = strlen(str);
    if (chr_count > 31) {
      chr_count = 31;
    }
    for (size_t i = 0; i < chr_count; i++) {
      string_descriptor_buf[1 + i] = (uint16_t)str[i];
    }
  }

  string_descriptor_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return string_descriptor_buf;
}
