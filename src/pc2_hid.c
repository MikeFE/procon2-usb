/*
  pc2_hid: IOHIDManager monitoring and report descriptor dumps.
*/
#include "pc2_hid.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <dispatch/dispatch.h>
#include <signal.h>
#include <string.h>

#include "s2_proto.h"

#define PC2_REPORT_BUFFER_SIZE  512
#define PC2_MAX_DEVICES         4   /* monitored at once */

/* Report descriptor parsing */

static const char *PC2_GetMainItemName(uint8_t tag)
{
    switch (tag) {
    case 0x8: return "Input";
    case 0x9: return "Output";
    case 0xB: return "Feature";
    case 0xA: return "Collection";
    case 0xC: return "End Collection";
    default: return "Main (reserved)";
    }
}

static const char *PC2_GetGlobalItemName(uint8_t tag)
{
    switch (tag) {
    case 0x0: return "Usage Page";
    case 0x1: return "Logical Minimum";
    case 0x2: return "Logical Maximum";
    case 0x3: return "Physical Minimum";
    case 0x4: return "Physical Maximum";
    case 0x5: return "Unit Exponent";
    case 0x6: return "Unit";
    case 0x7: return "Report Size";
    case 0x8: return "Report ID";
    case 0x9: return "Report Count";
    case 0xA: return "Push";
    case 0xB: return "Pop";
    default: return "Global (reserved)";
    }
}

static const char *PC2_GetLocalItemName(uint8_t tag)
{
    switch (tag) {
    case 0x0: return "Usage";
    case 0x1: return "Usage Minimum";
    case 0x2: return "Usage Maximum";
    case 0x3: return "Designator Index";
    case 0x4: return "Designator Minimum";
    case 0x5: return "Designator Maximum";
    case 0x7: return "String Index";
    case 0x8: return "String Minimum";
    case 0x9: return "String Maximum";
    case 0xA: return "Delimiter";
    default: return "Local (reserved)";
    }
}

/* 0x00FF is reserved by the HID spec, but this controller uses it as a
   vendor page, as it does 0xFF00+ */
static bool PC2_IsVendorPage(uint32_t page)
{
    return page == 0xFF || page >= 0xFF00;
}

static PC2_ReportSummary *PC2_GetReportSummary(PC2_DescriptorSummary *summary, uint8_t id)
{
    for (int i = 0; i < summary->num_reports; i++) {
        if (summary->reports[i].id == id) {
            return &summary->reports[i];
        }
    }
    if (summary->num_reports < PC2_MAX_REPORTS) {
        PC2_ReportSummary *report = &summary->reports[summary->num_reports++];
        report->id = id;
        return report;
    }
    return NULL;
}

static const char *PC2_GetUsagePageName(uint32_t page)
{
    switch (page) {
    case 0x01: return "Generic Desktop";
    case 0x02: return "Simulation";
    case 0x05: return "Game Controls";
    case 0x06: return "Generic Device";
    case 0x07: return "Keyboard";
    case 0x08: return "LED";
    case 0x09: return "Button";
    case 0x0C: return "Consumer";
    case 0x0F: return "Physical Interface";
    default:
        return PC2_IsVendorPage(page) ? "Vendor Defined" : NULL;
    }
}

static const char *PC2_GetDesktopUsageName(uint32_t usage)
{
    switch (usage) {
    case 0x01: return "Pointer";
    case 0x04: return "Joystick";
    case 0x05: return "Game Pad";
    case 0x08: return "Multi-axis Controller";
    case 0x30: return "X";
    case 0x31: return "Y";
    case 0x32: return "Z";
    case 0x33: return "Rx";
    case 0x34: return "Ry";
    case 0x35: return "Rz";
    case 0x36: return "Slider";
    case 0x37: return "Dial";
    case 0x38: return "Wheel";
    case 0x39: return "Hat Switch";
    default: return NULL;
    }
}

PC2_DescriptorKind PC2_ParseReportDescriptor(const uint8_t *data, size_t size,
                                             PC2_DescriptorSummary *summary, FILE *out)
{
    uint32_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint8_t report_id = 0;
    uint32_t stack[8];
    int stack_depth = 0;
    int depth = 0;
    bool have_top_usage = false;
    size_t i = 0;

    memset(summary, 0, sizeof(*summary));

    while (i < size) {
        uint8_t prefix = data[i];
        uint8_t item_size, type, tag;
        uint32_t value = 0;
        size_t start = i;

        if (prefix == 0xFE) {
            /* Long item: [0xFE, size, tag, data...] */
            item_size = (i + 1 < size) ? data[i + 1] : 0;
            if (out) {
                fprintf(out, "  %*s(long item, %u bytes)\n", depth * 2, "", item_size);
            }
            i += 3 + (size_t)item_size;
            continue;
        }

        item_size = prefix & 0x03;
        if (item_size == 3) {
            item_size = 4;
        }
        type = (prefix >> 2) & 0x03;
        tag = (prefix >> 4) & 0x0F;
        if (i + 1 + item_size > size) {
            if (out) {
                fprintf(out, "  (truncated item at offset %zu)\n", i);
            }
            break;
        }
        for (uint8_t b = 0; b < item_size; b++) {
            value |= (uint32_t)data[i + 1 + b] << (8 * b);
        }
        i += 1 + (size_t)item_size;

        if (type == 0 && tag == 0xC && depth > 0) {
            depth--;
        }

        if (out) {
            const char *name;
            const char *detail = NULL;
            char hex[16] = "";
            size_t len = 0;

            for (size_t b = start; b < i && len + 3 < sizeof(hex); b++) {
                len += (size_t)snprintf(hex + len, sizeof(hex) - len, "%02X ", data[b]);
            }

            switch (type) {
            case 0: name = PC2_GetMainItemName(tag); break;
            case 1: name = PC2_GetGlobalItemName(tag); break;
            case 2: name = PC2_GetLocalItemName(tag); break;
            default: name = "Reserved"; break;
            }
            if (type == 1 && tag == 0x0) {
                detail = PC2_GetUsagePageName(value);
            } else if (type == 2 && tag == 0x0 && usage_page == 0x01) {
                detail = PC2_GetDesktopUsageName(value);
            }

            fprintf(out, "  %-15s %*s%s", hex, depth * 2, "", name);
            if (type == 0 && tag == 0xC) {
                fprintf(out, "\n");
            } else if (type == 0 && (tag == 0x8 || tag == 0x9 || tag == 0xB)) {
                fprintf(out, " (%s, %s, %s)\n",
                        (value & 0x01) ? "Const" : "Data",
                        (value & 0x02) ? "Var" : "Array",
                        (value & 0x04) ? "Rel" : "Abs");
            } else if (detail) {
                fprintf(out, " (%s)\n", detail);
            } else {
                fprintf(out, " (0x%X)\n", value);
            }
        }

        if (type == 1) {
            switch (tag) {
            case 0x0:
                usage_page = value;
                break;
            case 0x7:
                report_size = value;
                break;
            case 0x8:
                report_id = (uint8_t)value;
                summary->report_ids++;
                break;
            case 0x9:
                report_count = value;
                break;
            case 0xA:
                if (stack_depth < (int)(sizeof(stack) / sizeof(stack[0]))) {
                    stack[stack_depth++] = usage_page;
                }
                break;
            case 0xB:
                if (stack_depth > 0) {
                    usage_page = stack[--stack_depth];
                }
                break;
            default:
                break;
            }
        } else if (type == 2 && tag == 0x0 && !have_top_usage) {
            /* Extended usages carry their own page in the high 16 bits */
            summary->top_usage_page = (item_size == 4) ? (value >> 16) : usage_page;
            summary->top_usage = value & 0xFFFF;
            have_top_usage = true;
        } else if (type == 0) {
            if (tag == 0xA) {
                depth++;
            } else if (tag == 0x8) {
                PC2_ReportSummary *report = PC2_GetReportSummary(summary, report_id);
                bool constant = (value & 0x01) != 0;

                if (report) {
                    report->input_bits += report_size * report_count;
                }
                if (constant) {
                    /* padding */
                } else if (usage_page == 0x01 || usage_page == 0x09) {
                    summary->standard_inputs++;
                    if (report) {
                        report->standard_inputs++;
                    }
                } else if (PC2_IsVendorPage(usage_page)) {
                    summary->vendor_inputs++;
                    if (report) {
                        report->vendor_inputs++;
                    }
                } else {
                    summary->other_inputs++;
                }
            }
        }
    }

    if (summary->standard_inputs && summary->vendor_inputs) {
        return PC2_DESCRIPTOR_MIXED;
    } else if (summary->standard_inputs) {
        return PC2_DESCRIPTOR_STANDARD;
    } else if (summary->vendor_inputs) {
        return PC2_DESCRIPTOR_VENDOR;
    }
    return PC2_DESCRIPTOR_EMPTY;
}

const char *PC2_GetDescriptorKindString(PC2_DescriptorKind kind)
{
    switch (kind) {
    case PC2_DESCRIPTOR_STANDARD:
        return "standard gamepad usages";
    case PC2_DESCRIPTOR_VENDOR:
        return "vendor-defined usages only";
    case PC2_DESCRIPTOR_MIXED:
        return "mixed standard and vendor-defined usages";
    default:
        return "no data inputs";
    }
}

/* Device enumeration */

IOHIDManagerRef PC2_HIDCreateManager(void)
{
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int32_t vendor_id = S2_VENDOR_NINTENDO;
    int32_t product_id = S2_PRODUCT_PRO;
    CFNumberRef vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vendor_id);
    CFNumberRef pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &product_id);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *values[] = { vid, pid };
    CFDictionaryRef matching = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);

    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);
    CFRelease(vid);
    CFRelease(pid);
    return manager;
}

int32_t PC2_GetDeviceInt(IOHIDDeviceRef device, CFStringRef key)
{
    int32_t value = 0;
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    if (ref && CFGetTypeID(ref) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
    }
    return value;
}

void PC2_GetDeviceString(IOHIDDeviceRef device, CFStringRef key, char *buffer, size_t size)
{
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    buffer[0] = '\0';
    if (ref && CFGetTypeID(ref) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)ref, buffer, (CFIndex)size, kCFStringEncodingUTF8);
    }
}

static void PC2_DumpDevice(IOHIDDeviceRef device, FILE *out, bool raw_only)
{
    char product[128], manufacturer[128], transport[32];
    CFTypeRef descriptor = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDReportDescriptorKey));

    PC2_GetDeviceString(device, CFSTR(kIOHIDProductKey), product, sizeof(product));
    PC2_GetDeviceString(device, CFSTR(kIOHIDManufacturerKey), manufacturer, sizeof(manufacturer));
    PC2_GetDeviceString(device, CFSTR(kIOHIDTransportKey), transport, sizeof(transport));

    fprintf(out, "Device: %s / %s (%04X:%04X) over %s\n", manufacturer, product,
            (unsigned)PC2_GetDeviceInt(device, CFSTR(kIOHIDVendorIDKey)),
            (unsigned)PC2_GetDeviceInt(device, CFSTR(kIOHIDProductIDKey)),
            transport[0] ? transport : "?");
    fprintf(out, "  Version: 0x%04X  Location: 0x%08X\n",
            (unsigned)PC2_GetDeviceInt(device, CFSTR(kIOHIDVersionNumberKey)),
            (unsigned)PC2_GetDeviceInt(device, CFSTR(kIOHIDLocationIDKey)));
    fprintf(out, "  Primary usage: page 0x%X usage 0x%X\n",
            (unsigned)PC2_GetDeviceInt(device, CFSTR(kIOHIDPrimaryUsagePageKey)),
            (unsigned)PC2_GetDeviceInt(device, CFSTR(kIOHIDPrimaryUsageKey)));
    fprintf(out, "  Max report sizes: input %d, output %d, feature %d\n",
            PC2_GetDeviceInt(device, CFSTR(kIOHIDMaxInputReportSizeKey)),
            PC2_GetDeviceInt(device, CFSTR(kIOHIDMaxOutputReportSizeKey)),
            PC2_GetDeviceInt(device, CFSTR(kIOHIDMaxFeatureReportSizeKey)));

    if (!descriptor || CFGetTypeID(descriptor) != CFDataGetTypeID()) {
        fprintf(out, "  (no report descriptor property)\n\n");
        return;
    }

    const uint8_t *bytes = CFDataGetBytePtr((CFDataRef)descriptor);
    size_t size = (size_t)CFDataGetLength((CFDataRef)descriptor);
    PC2_DescriptorSummary summary;
    PC2_DescriptorKind kind;

    fprintf(out, "  Report descriptor (%zu bytes):\n", size);
    for (size_t i = 0; i < size; i++) {
        fprintf(out, "%s%02X", (i % 16 == 0) ? "    " : " ", bytes[i]);
        if (i % 16 == 15 || i + 1 == size) {
            fprintf(out, "\n");
        }
    }
    if (raw_only) {
        fprintf(out, "\n");
        return;
    }

    fprintf(out, "  Decoded:\n");
    kind = PC2_ParseReportDescriptor(bytes, size, &summary, out);
    fprintf(out, "  Top-level usage: page 0x%X usage 0x%X\n", summary.top_usage_page, summary.top_usage);
    fprintf(out, "  Inputs: %d standard, %d vendor, %d other; %d report ID(s)\n",
            summary.standard_inputs, summary.vendor_inputs, summary.other_inputs, summary.report_ids);
    for (int i = 0; i < summary.num_reports; i++) {
        const PC2_ReportSummary *report = &summary.reports[i];
        fprintf(out, "  Input report 0x%02X: %u bytes + ID, %s\n", report->id, (report->input_bits + 7) / 8,
                report->standard_inputs ? (report->vendor_inputs ? "standard + vendor fields" : "standard fields")
                                        : "vendor fields only");
    }
    fprintf(out, "  Verdict: %s\n\n", PC2_GetDescriptorKindString(kind));
}

typedef struct PC2_DumpContext
{
    FILE *out;
    bool raw_only;
    int count;
} PC2_DumpContext;

static void PC2_DumpDeviceApplier(const void *value, void *context)
{
    PC2_DumpContext *ctx = (PC2_DumpContext *)context;
    PC2_DumpDevice((IOHIDDeviceRef)value, ctx->out, ctx->raw_only);
    ctx->count++;
}

int PC2_HIDDumpDescriptors(FILE *out, bool raw_only)
{
    IOHIDManagerRef manager = PC2_HIDCreateManager();
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    PC2_DumpContext ctx = { out, raw_only, 0 };

    if (devices) {
        CFSetApplyFunction(devices, PC2_DumpDeviceApplier, &ctx);
        CFRelease(devices);
    }
    CFRelease(manager);
    return ctx.count;
}

/* Report probe */

typedef struct PC2_ProbeContext
{
    bool got_report;
    uint8_t buffer[PC2_REPORT_BUFFER_SIZE];
} PC2_ProbeContext;

static void PC2_ProbeReportCallback(void *context, IOReturn result, void *sender, IOHIDReportType type,
                                    uint32_t report_id, uint8_t *report, CFIndex report_length)
{
    PC2_ProbeContext *ctx = (PC2_ProbeContext *)context;

    (void)sender;
    (void)type;
    (void)report_id;
    (void)report;
    if (result == kIOReturnSuccess && report_length > 0) {
        ctx->got_report = true;
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

static void PC2_ProbeMatchedCallback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    PC2_ProbeContext *ctx = (PC2_ProbeContext *)context;

    (void)result;
    (void)sender;
    IOHIDDeviceRegisterInputReportCallback(device, ctx->buffer, sizeof(ctx->buffer),
                                           PC2_ProbeReportCallback, ctx);
}

/* A private run loop mode, so callers' own run loop sources (the agent's
   timers and IOKit notifications) don't fire while we wait */
#define PC2_PROBE_RUN_LOOP_MODE CFSTR("io.github.procon2-usb.probe")

bool PC2_HIDWaitForReport(double timeout_s)
{
    PC2_ProbeContext ctx;
    IOHIDManagerRef manager = PC2_HIDCreateManager();
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + timeout_s;

    ctx.got_report = false;
    IOHIDManagerRegisterDeviceMatchingCallback(manager, PC2_ProbeMatchedCallback, &ctx);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), PC2_PROBE_RUN_LOOP_MODE);
    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
        while (!ctx.got_report) {
            CFTimeInterval remaining = deadline - CFAbsoluteTimeGetCurrent();
            if (remaining <= 0) {
                break;
            }
            CFRunLoopRunInMode(PC2_PROBE_RUN_LOOP_MODE, remaining, false);
        }
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    }
    IOHIDManagerUnscheduleFromRunLoop(manager, CFRunLoopGetCurrent(), PC2_PROBE_RUN_LOOP_MODE);
    CFRelease(manager);
    return ctx.got_report;
}

/* Live monitoring */

typedef struct PC2_MonitorDevice
{
    IOHIDDeviceRef device;
    bool raw;
    bool have_last;
    size_t last_size;
    uint32_t last_report_id;
    uint32_t report_count;
    uint8_t last[PC2_REPORT_BUFFER_SIZE];
    uint8_t buffer[PC2_REPORT_BUFFER_SIZE];
} PC2_MonitorDevice;

typedef struct PC2_MonitorContext
{
    bool raw;
    PC2_MonitorDevice devices[PC2_MAX_DEVICES];
} PC2_MonitorContext;

static void PC2_PrintRawReport(PC2_MonitorDevice *dev, uint32_t report_id, const uint8_t *report, size_t size)
{
    /* Print only when something other than the timestamp/IMU bytes changes;
       mark changed bytes with '*' so button positions are easy to find. */
    bool changed = !dev->have_last || size != dev->last_size;
    for (size_t i = 0; i < size && i < 0x20 && !changed; i++) {
        changed = (report[i] != dev->last[i]);
    }
    if (!changed) {
        return;
    }

    printf("id=%02X len=%zu:", report_id, size);
    for (size_t i = 0; i < size && i < 0x20; i++) {
        bool diff = dev->have_last && report[i] != dev->last[i];
        printf("%s%02X", diff ? "*" : " ", report[i]);
    }
    printf("\n");
}

static void PC2_PrintProState(const uint8_t *report, size_t size)
{
    S2_ProState state;
    char names[160] = "";
    size_t len = 0;

    if (!S2_ParseProReport(&state, report, size)) {
        printf("\r\033[K(short report: %zu bytes)", size);
        fflush(stdout);
        return;
    }

    for (int bit = 0; bit < 32; bit++) {
        const char *name;
        if (!(state.buttons & (1u << bit))) {
            continue;
        }
        name = S2_GetProButtonName(1u << bit);
        if (name && len + strlen(name) + 2 < sizeof(names)) {
            len += (size_t)snprintf(names + len, sizeof(names) - len, "%s ", name);
        }
    }

    printf("\r\033[KL(%4u,%4u) R(%4u,%4u) LX%+6d LY%+6d RX%+6d RY%+6d  [%s]",
           state.left_x, state.left_y, state.right_x, state.right_y,
           S2_MapStickAxis(NULL, state.left_x, false),
           S2_MapStickAxis(NULL, state.left_y, true),
           S2_MapStickAxis(NULL, state.right_x, false),
           S2_MapStickAxis(NULL, state.right_y, true),
           names);
    fflush(stdout);
}

static void PC2_PrintHIDState(const uint8_t *report, size_t size)
{
    S2_HIDState state;
    char names[160] = "";
    size_t len = 0;

    if (!S2_ParseHIDReport(&state, report, size)) {
        return;
    }
    for (int i = 0; i < S2_HID_NUM_BUTTONS; i++) {
        const char *name = S2_GetHIDButtonName(i);
        if ((state.buttons & (1u << i)) && len + strlen(name) + 2 < sizeof(names)) {
            len += (size_t)snprintf(names + len, sizeof(names) - len, "%s ", name);
        }
    }
    printf("\r\033[KX%5u Y%5u Rx%5u Rz%5u  [%s]",
           state.axes[0], state.axes[1], state.axes[2], state.axes[3], names);
    fflush(stdout);
}

static void PC2_InputReportCallback(void *context, IOReturn result, void *sender, IOHIDReportType type,
                                    uint32_t report_id, uint8_t *report, CFIndex report_length)
{
    PC2_MonitorDevice *dev = (PC2_MonitorDevice *)context;
    uint8_t normalized[PC2_REPORT_BUFFER_SIZE + 1];
    const uint8_t *data = report;
    size_t size = (size_t)report_length;

    (void)sender;
    (void)type;
    if (result != kIOReturnSuccess || size == 0) {
        return;
    }

    /* IOHIDDevice includes the report ID byte when the device uses report
       IDs. Without one, prepend a zero so offsets match hidapi/SDL. */
    if (report_id == 0 && size < sizeof(normalized)) {
        normalized[0] = 0;
        memcpy(normalized + 1, report, size);
        data = normalized;
        size++;
    }

    if (dev->report_count++ == 0 || report_id != dev->last_report_id) {
        printf("\nInput report id=0x%02X, %zu bytes\n", report_id, size);
        dev->last_report_id = report_id;
    }

    if (dev->raw) {
        PC2_PrintRawReport(dev, report_id, data, size);
    } else if (data[0] == S2_HID_REPORT_ID) {
        PC2_PrintHIDState(data, size);
    } else {
        PC2_PrintProState(data, size);
    }

    if (size <= sizeof(dev->last)) {
        memcpy(dev->last, data, size);
        dev->last_size = size;
        dev->have_last = true;
    }
}

static void PC2_DeviceMatchedCallback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    PC2_MonitorContext *ctx = (PC2_MonitorContext *)context;
    char product[128];

    (void)result;
    (void)sender;
    for (int i = 0; i < PC2_MAX_DEVICES; i++) {
        PC2_MonitorDevice *dev = &ctx->devices[i];
        if (dev->device) {
            continue;
        }
        memset(dev, 0, sizeof(*dev));
        dev->device = device;
        dev->raw = ctx->raw;
        PC2_GetDeviceString(device, CFSTR(kIOHIDProductKey), product, sizeof(product));
        printf("\nAttached: %s\n", product);
        IOHIDDeviceRegisterInputReportCallback(device, dev->buffer, sizeof(dev->buffer),
                                               PC2_InputReportCallback, dev);
        return;
    }
}

static void PC2_DeviceRemovedCallback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    PC2_MonitorContext *ctx = (PC2_MonitorContext *)context;

    (void)result;
    (void)sender;
    for (int i = 0; i < PC2_MAX_DEVICES; i++) {
        if (ctx->devices[i].device == device) {
            ctx->devices[i].device = NULL;
            printf("\nDetached\n");
        }
    }
}

int PC2_HIDMonitor(bool raw)
{
    static PC2_MonitorContext ctx;
    IOHIDManagerRef manager = PC2_HIDCreateManager();
    dispatch_source_t sigint;
    IOReturn result;

    memset(&ctx, 0, sizeof(ctx));
    ctx.raw = raw;

    IOHIDManagerRegisterDeviceMatchingCallback(manager, PC2_DeviceMatchedCallback, &ctx);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, PC2_DeviceRemovedCallback, &ctx);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    result = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "error: couldn't open the HID manager (0x%08X)\n", result);
        if (result == kIOReturnNotPermitted) {
            fprintf(stderr, "  Grant Input Monitoring to your terminal in System Settings > Privacy & Security.\n");
        }
        CFRelease(manager);
        return 1;
    }

    printf("Monitoring %s HID input. Press Ctrl-C to stop.\n", S2_GetProductName(S2_PRODUCT_PRO));
    signal(SIGINT, SIG_IGN);
    sigint = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sigint, ^{
        CFRunLoopStop(CFRunLoopGetMain());
    });
    dispatch_resume(sigint);
    CFRunLoopRun();
    dispatch_source_cancel(sigint);
    dispatch_release(sigint);
    printf("\n");

    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    return 0;
}
