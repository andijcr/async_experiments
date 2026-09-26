// TinyUSB's own required application-supplied config header (tusb.h
// unconditionally #includes "tusb_config.h" - not optional). Owned by
// estrp2040 itself, not the consuming example/project: this encodes
// "what classes/buffer sizes does estrp2040::Serial provide" (the
// mechanism), which is fixed by this module's own design - unlike the
// USB device/configuration/string descriptors (product identity: VID/
// PID, product strings), which stay the consuming application's own
// concern (examples/rp2040/src/usb_descriptors.c has this project's own
// demo ones). CFG_TUSB_MCU/CFG_TUSB_OS are already supplied by
// pico-sdk's own tinyusb_device CMake target as compile definitions -
// not repeated here.
#ifndef ESTRP2040_TUSB_CONFIG_H
#define ESTRP2040_TUSB_CONFIG_H

// NOLINTBEGIN(cppcoreguidelines-macro-usage) - TinyUSB's own headers
// check these via #ifdef/#if, a plain C preprocessor contract this C++
// project can't sidestep with constexpr here.
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
// NOLINTEND(cppcoreguidelines-macro-usage)

#endif
