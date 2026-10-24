/*
  pc2_usb: send the Switch 2 init sequence over the vendor bulk interface
  using IOKit IOUSBLib. No entitlements needed for a real device.
*/
#ifndef PC2_USB_H
#define PC2_USB_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>
#include <stdint.h>

#include "s2_proto.h"

typedef enum PC2_Status
{
    PC2_OK = 0,
    PC2_ERR_NOT_FOUND,
    PC2_ERR_PLUGIN,
    PC2_ERR_OPEN,
    PC2_ERR_EXCLUSIVE,
    PC2_ERR_NOT_PERMITTED,
    PC2_ERR_NO_ENDPOINTS,
    PC2_ERR_COMMAND
} PC2_Status;

typedef struct PC2_USBContext
{
    io_service_t service;
    IOUSBInterfaceInterface800 **intf;
    bool opened;
    uint16_t product_id;
    uint64_t location_id;
    UInt8 pipe_out;
    UInt8 pipe_in;      /* 0 if the interface has no bulk IN pipe */
    uint8_t report_format; /* overrides the sequence's report format if nonzero */
    IOReturn last_error;
    int failed_command; /* index into the init sequence, -1 if none */
} PC2_USBContext;

/* Called after each init command. `reply` is NULL if nothing came back. */
typedef void (*PC2_ReplyCallback)(void *userdata, size_t index, const S2_Command *command,
                                  const uint8_t *reply, uint32_t reply_size);

void PC2_USBInit(PC2_USBContext *ctx);

/* Product IDs the enabler looks for */
size_t PC2_USBGetProductIDs(const uint16_t **product_ids);

/* Find the first supported controller's command interface. The caller owns
   the returned service reference (or IO_OBJECT_NULL). */
io_service_t PC2_USBFindCommandInterface(uint16_t *product_id);

/* Build an IOKit matching dictionary for one product's command interface */
CFMutableDictionaryRef PC2_USBCreateMatching(uint16_t product_id);

/* Open the interface `service` (retained by the context) and resolve pipes */
PC2_Status PC2_USBOpen(PC2_USBContext *ctx, io_service_t service);

/* Send the full init sequence, draining one reply per command */
PC2_Status PC2_USBSendInit(PC2_USBContext *ctx, PC2_ReplyCallback callback, void *userdata);

/* Send any command sequence, draining one reply per command. The report
   format override applies here too. */
PC2_Status PC2_USBSendSequence(PC2_USBContext *ctx, const S2_Command *commands, size_t count,
                               PC2_ReplyCallback callback, void *userdata);

void PC2_USBClose(PC2_USBContext *ctx);

const char *PC2_GetStatusString(PC2_Status status);

#endif /* PC2_USB_H */
