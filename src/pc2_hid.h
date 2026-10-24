/*
  pc2_hid: IOHIDManager access to the controller's HID interface, which the
  built-in macOS HID driver owns. Read-only: nothing is seized or created.
*/
#ifndef PC2_HID_H
#define PC2_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <IOKit/hid/IOHIDManager.h>

typedef enum PC2_DescriptorKind
{
    PC2_DESCRIPTOR_EMPTY = 0,
    PC2_DESCRIPTOR_STANDARD,    /* buttons/axes declared with standard usages */
    PC2_DESCRIPTOR_VENDOR,      /* inputs only under vendor-defined usage pages */
    PC2_DESCRIPTOR_MIXED
} PC2_DescriptorKind;

#define PC2_MAX_REPORTS     8

typedef struct PC2_ReportSummary
{
    uint8_t id;
    uint32_t input_bits;    /* excluding the report ID byte */
    int standard_inputs;
    int vendor_inputs;
} PC2_ReportSummary;

typedef struct PC2_DescriptorSummary
{
    int standard_inputs;    /* Input items under Generic Desktop or Button pages */
    int vendor_inputs;      /* Input items under vendor pages (0xFF00+, and 0x00FF) */
    int other_inputs;
    int report_ids;
    uint32_t top_usage_page;
    uint32_t top_usage;
    int num_reports;        /* reports with Input items */
    PC2_ReportSummary reports[PC2_MAX_REPORTS];
} PC2_DescriptorSummary;

/* A manager matching the Pro Controller's HID interface. Caller releases. */
IOHIDManagerRef PC2_HIDCreateManager(void);

int32_t PC2_GetDeviceInt(IOHIDDeviceRef device, CFStringRef key);
void PC2_GetDeviceString(IOHIDDeviceRef device, CFStringRef key, char *buffer, size_t size);

/* Decode a report descriptor. If `out` is non-NULL, print one line per item. */
PC2_DescriptorKind PC2_ParseReportDescriptor(const uint8_t *data, size_t size,
                                             PC2_DescriptorSummary *summary, FILE *out);

const char *PC2_GetDescriptorKindString(PC2_DescriptorKind kind);

/* Print every matching HID device's properties and report descriptor.
   Returns the number of devices found. */
int PC2_HIDDumpDescriptors(FILE *out, bool raw_only);

/* Wait up to `timeout_s` for any input report. Returns true if one arrived. */
bool PC2_HIDWaitForReport(double timeout_s);

/* Print live state until interrupted. `raw` prints changed report bytes
   instead of decoded state. Returns a process exit code. */
int PC2_HIDMonitor(bool raw);

/* Guided button/axis mapping that ends with an SDL mapping string.
   Returns a process exit code. */
int PC2_HIDLearn(void);

#endif /* PC2_HID_H */
