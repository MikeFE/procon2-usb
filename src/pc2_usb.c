/*
  pc2_usb: IOUSBLib transport for the Switch 2 USB init sequence.
*/
#include "pc2_usb.h"

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/USB.h>
#include <string.h>
#include <unistd.h>

#define PC2_WRITE_TIMEOUT_MS    1000
#define PC2_READ_TIMEOUT_MS     250
#define PC2_MAX_STALE_REPLIES   4   /* late replies to skip before giving up */
#define PC2_COMMAND_DELAY_US    5000

/* Product IDs to look for, in order. Extend as other controllers are tested. */
static const uint16_t pc2_product_ids[] = {
    S2_PRODUCT_PRO,
};

static void PC2_SetNumber(CFMutableDictionaryRef dict, CFStringRef key, SInt32 value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

void PC2_USBInit(PC2_USBContext *ctx)
{
    ctx->service = IO_OBJECT_NULL;
    ctx->intf = NULL;
    ctx->opened = false;
    ctx->product_id = 0;
    ctx->location_id = 0;
    ctx->pipe_out = 0;
    ctx->pipe_in = 0;
    ctx->report_format = 0;
    ctx->last_error = kIOReturnSuccess;
    ctx->failed_command = -1;
}

CFMutableDictionaryRef PC2_USBCreateMatching(uint16_t product_id)
{
    /* idVendor/idProduct at the top level no longer match interfaces from
       userspace; IOPropertyMatch does. */
    CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostInterface");
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    if (!matching || !props) {
        if (matching) {
            CFRelease(matching);
        }
        if (props) {
            CFRelease(props);
        }
        return NULL;
    }
    PC2_SetNumber(props, CFSTR(kUSBVendorID), S2_VENDOR_NINTENDO);
    PC2_SetNumber(props, CFSTR(kUSBProductID), product_id);
    PC2_SetNumber(props, CFSTR(kUSBInterfaceNumber), S2_USB_COMMAND_INTERFACE);
    CFDictionarySetValue(matching, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);
    return matching;
}

size_t PC2_USBGetProductIDs(const uint16_t **product_ids)
{
    *product_ids = pc2_product_ids;
    return sizeof(pc2_product_ids) / sizeof(pc2_product_ids[0]);
}

io_service_t PC2_USBFindCommandInterface(uint16_t *product_id)
{
    for (size_t i = 0; i < sizeof(pc2_product_ids) / sizeof(pc2_product_ids[0]); i++) {
        CFMutableDictionaryRef matching = PC2_USBCreateMatching(pc2_product_ids[i]);
        if (!matching) {
            continue;
        }
        /* Consumes the matching reference */
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
        if (service != IO_OBJECT_NULL) {
            if (product_id) {
                *product_id = pc2_product_ids[i];
            }
            return service;
        }
    }
    return IO_OBJECT_NULL;
}

static uint64_t PC2_GetRegistryNumber(io_service_t service, CFStringRef key)
{
    uint64_t value = 0;
    CFTypeRef ref = IORegistryEntrySearchCFProperty(service, kIOServicePlane, key, kCFAllocatorDefault,
                                                    kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (ref) {
        if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, &value);
        }
        CFRelease(ref);
    }
    return value;
}

static PC2_Status PC2_StatusFromOpenError(IOReturn result)
{
    switch (result) {
    case kIOReturnExclusiveAccess:
        return PC2_ERR_EXCLUSIVE;
    case kIOReturnNotPermitted:
    case kIOReturnNotPrivileged:
        return PC2_ERR_NOT_PERMITTED;
    default:
        return PC2_ERR_OPEN;
    }
}

static PC2_Status PC2_ResolvePipes(PC2_USBContext *ctx)
{
    IOUSBInterfaceInterface800 **intf = ctx->intf;
    UInt8 num_endpoints = 0;
    IOReturn result;

    result = (*intf)->GetNumEndpoints(intf, &num_endpoints);
    if (result != kIOReturnSuccess) {
        ctx->last_error = result;
        return PC2_ERR_NO_ENDPOINTS;
    }

    /* Pipe refs are 1-based; 0 is the default control pipe */
    for (UInt8 pipe = 1; pipe <= num_endpoints; pipe++) {
        UInt8 direction, number, transfer_type, interval;
        UInt16 max_packet_size;

        result = (*intf)->GetPipeProperties(intf, pipe, &direction, &number, &transfer_type,
                                            &max_packet_size, &interval);
        if (result != kIOReturnSuccess || transfer_type != kUSBBulk) {
            continue;
        }
        if (direction == kUSBOut && !ctx->pipe_out) {
            ctx->pipe_out = pipe;
        } else if (direction == kUSBIn && !ctx->pipe_in) {
            ctx->pipe_in = pipe;
        }
    }

    return ctx->pipe_out ? PC2_OK : PC2_ERR_NO_ENDPOINTS;
}

PC2_Status PC2_USBOpen(PC2_USBContext *ctx, io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn result;
    HRESULT hr;

    ctx->service = service;
    IOObjectRetain(service);
    ctx->product_id = (uint16_t)PC2_GetRegistryNumber(service, CFSTR(kUSBProductID));
    ctx->location_id = PC2_GetRegistryNumber(service, CFSTR(kUSBDevicePropertyLocationID));

    result = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
    if (result != kIOReturnSuccess || !plugin) {
        ctx->last_error = result;
        return PC2_ERR_PLUGIN;
    }

    hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID800),
                                   (LPVOID *)&ctx->intf);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK || !ctx->intf) {
        ctx->intf = NULL;
        ctx->last_error = kIOReturnUnsupported;
        return PC2_ERR_PLUGIN;
    }

    result = (*ctx->intf)->USBInterfaceOpen(ctx->intf);
    if (result != kIOReturnSuccess) {
        ctx->last_error = result;
        return PC2_StatusFromOpenError(result);
    }
    ctx->opened = true;

    return PC2_ResolvePipes(ctx);
}

/* Read until the reply to `command` arrives, skipping late replies to
   earlier commands. Returns the reply size, or 0 if none came. */
static UInt32 PC2_ReadReply(PC2_USBContext *ctx, const uint8_t *command, uint8_t *reply, UInt32 size)
{
    IOUSBInterfaceInterface800 **intf = ctx->intf;

    for (int attempt = 0; attempt <= PC2_MAX_STALE_REPLIES; attempt++) {
        UInt32 reply_size = size;
        IOReturn result = (*intf)->ReadPipeTO(intf, ctx->pipe_in, reply, &reply_size,
                                              PC2_READ_TIMEOUT_MS, PC2_READ_TIMEOUT_MS);
        if (result == kIOUSBPipeStalled) {
            (*intf)->ClearPipeStallBothEnds(intf, ctx->pipe_in);
        }
        if (result != kIOReturnSuccess) {
            return 0;
        }
        if (S2_IsReplyTo(command, reply, reply_size)) {
            return reply_size;
        }
    }
    return 0;
}

PC2_Status PC2_USBSendInit(PC2_USBContext *ctx, PC2_ReplyCallback callback, void *userdata)
{
    const S2_Command *commands;
    size_t count = S2_GetUSBInitSequence(&commands);

    return PC2_USBSendSequence(ctx, commands, count, callback, userdata);
}

PC2_Status PC2_USBSendSequence(PC2_USBContext *ctx, const S2_Command *commands, size_t count,
                               PC2_ReplyCallback callback, void *userdata)
{
    IOUSBInterfaceInterface800 **intf = ctx->intf;
    uint8_t buffer[S2_CMD_MAX_SIZE];
    uint8_t reply[S2_REPLY_MAX_SIZE];

    ctx->failed_command = -1;

    for (size_t i = 0; i < count; i++) {
        const S2_Command *command = &commands[i];
        UInt32 reply_size = 0;
        IOReturn result;

        /* WritePipe takes a non-const buffer */
        memcpy(buffer, command->data, command->size);
        if (ctx->report_format && S2_IsReportFormatCommand(buffer)) {
            buffer[S2_REPORT_FORMAT_OFFSET] = ctx->report_format;
        }
        result = (*intf)->WritePipeTO(intf, ctx->pipe_out, buffer, (UInt32)command->size,
                                      PC2_WRITE_TIMEOUT_MS, PC2_WRITE_TIMEOUT_MS);
        if (result == kIOUSBPipeStalled) {
            (*intf)->ClearPipeStallBothEnds(intf, ctx->pipe_out);
            result = (*intf)->WritePipeTO(intf, ctx->pipe_out, buffer, (UInt32)command->size,
                                          PC2_WRITE_TIMEOUT_MS, PC2_WRITE_TIMEOUT_MS);
        }
        if (result != kIOReturnSuccess) {
            ctx->last_error = result;
            ctx->failed_command = (int)i;
            return PC2_ERR_COMMAND;
        }

        if (ctx->pipe_in) {
            reply_size = PC2_ReadReply(ctx, buffer, reply, sizeof(reply));
        }

        if (callback) {
            callback(userdata, i, command, reply_size ? reply : NULL, reply_size);
        }
        usleep(PC2_COMMAND_DELAY_US);
    }

    return PC2_OK;
}

void PC2_USBClose(PC2_USBContext *ctx)
{
    if (ctx->intf) {
        if (ctx->opened) {
            (*ctx->intf)->USBInterfaceClose(ctx->intf);
        }
        (*ctx->intf)->Release(ctx->intf);
    }
    if (ctx->service != IO_OBJECT_NULL) {
        IOObjectRelease(ctx->service);
    }
    PC2_USBInit(ctx);
}

const char *PC2_GetStatusString(PC2_Status status)
{
    switch (status) {
    case PC2_OK:
        return "success";
    case PC2_ERR_NOT_FOUND:
        return "controller not found";
    case PC2_ERR_PLUGIN:
        return "couldn't create the IOUSBLib interface plug-in";
    case PC2_ERR_OPEN:
        return "couldn't open the command interface";
    case PC2_ERR_EXCLUSIVE:
        return "the command interface is held exclusively by another process";
    case PC2_ERR_NOT_PERMITTED:
        return "not permitted to open the command interface";
    case PC2_ERR_NO_ENDPOINTS:
        return "couldn't find the bulk OUT endpoint on the command interface";
    case PC2_ERR_COMMAND:
        return "an init command failed";
    default:
        return "unknown error";
    }
}
