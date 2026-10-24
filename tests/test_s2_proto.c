/*
  Offline tests for s2_proto. No hardware needed.
*/
#include <stdio.h>
#include <string.h>

#include "pc2_hid.h"
#include "s2_proto.h"

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void TestInitSequenceFraming(void)
{
    const S2_Command *commands;
    size_t count = S2_GetUSBInitSequence(&commands);

    CHECK(count == 10);
    for (size_t i = 0; i < count; i++) {
        CHECK(commands[i].size <= S2_CMD_MAX_SIZE);
        CHECK(commands[i].data[1] == 0x91);
        CHECK(S2_GetCommandSize(commands[i].data) == commands[i].size);
    }
    /* Report format 0x09: standard HID report, S2_ParseHIDReport() */
    CHECK(S2_IsReportFormatCommand(commands[8].data));
    CHECK(commands[8].data[S2_REPORT_FORMAT_OFFSET] == S2_REPORT_FORMAT_HID);
}

static void TestReplyMatching(void)
{
    const S2_Command *commands;
    const uint8_t reply_feature_mask[] = { 0x0C, 0x01, 0x00, 0x02, 0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0 };

    S2_GetUSBInitSequence(&commands);
    CHECK(S2_IsReplyTo(commands[1].data, reply_feature_mask, sizeof(reply_feature_mask)));
    /* A late reply to command 2 must not count as command 3's reply */
    CHECK(!S2_IsReplyTo(commands[2].data, reply_feature_mask, sizeof(reply_feature_mask)));
    /* 0C/02 and 0C/04 share a command byte; the subcommand tells them apart */
    CHECK(!S2_IsReplyTo(commands[4].data, reply_feature_mask, sizeof(reply_feature_mask)));
    CHECK(!S2_IsReplyTo(commands[1].data, reply_feature_mask, 4));
}

static void TestFlashRead(void)
{
    uint8_t cmd[S2_CMD_MAX_SIZE];
    size_t size = S2_BuildFlashRead(cmd, 0x1FC040);

    CHECK(size == 16);
    CHECK(S2_GetCommandSize(cmd) == size);
    CHECK(cmd[12] == 0x40 && cmd[13] == 0xC0 && cmd[14] == 0x1F && cmd[15] == 0x00);
}

static void TestParseProReport(void)
{
    uint8_t report[64];
    S2_ProState state;

    memset(report, 0, sizeof(report));
    CHECK(!S2_ParseProReport(&state, report, 63));

    report[0] = 0x09;
    report[5] = 0x08 | 0x80;    /* A, ZR */
    report[6] = 0x40;           /* C */
    report[7] = 0x02 | 0x40;    /* Up, L */
    report[8] = 0x03;           /* GR, GL */
    /* left (0x123, 0x456), right (0x800, 0xFFF) */
    report[11] = 0x23; report[12] = 0x61; report[13] = 0x45;
    report[14] = 0x00; report[15] = 0xF8; report[16] = 0xFF;

    CHECK(S2_ParseProReport(&state, report, sizeof(report)));
    CHECK(state.buttons == (S2_PRO_BUTTON_A | S2_PRO_BUTTON_ZR | S2_PRO_BUTTON_C | S2_PRO_BUTTON_UP |
                            S2_PRO_BUTTON_L | S2_PRO_BUTTON_GR | S2_PRO_BUTTON_GL));
    CHECK(state.left_x == 0x123);
    CHECK(state.left_y == 0x456);
    CHECK(state.right_x == 0x800);
    CHECK(state.right_y == 0xFFF);
    CHECK(strcmp(S2_GetProButtonName(S2_PRO_BUTTON_GL), "GL") == 0);
}

static void TestParseHIDReport(void)
{
    uint8_t report[64];
    S2_HIDState state;
    const S2_Command *commands;
    size_t count = S2_GetUSBInitSequence(&commands);
    int format_commands = 0;

    memset(report, 0, sizeof(report));
    report[0] = 0x05;
    CHECK(!S2_ParseHIDReport(&state, report, sizeof(report)));

    report[0] = 0x09;
    report[3] = 0x01;           /* button 1 */
    report[5] = 0xF0 | 0x10;    /* button 21 + padding bits */
    report[6] = 0x23; report[7] = 0x61; report[8] = 0x45;   /* X 0x123, Y 0x456 */
    report[9] = 0xFF; report[10] = 0x0F; report[11] = 0x80; /* Rx 0xFFF, Rz 0x800 */
    CHECK(S2_ParseHIDReport(&state, report, sizeof(report)));
    CHECK(state.buttons == (1u | (1u << 20)));
    CHECK(strcmp(S2_GetHIDButtonName(S2_HID_BUTTON_B), "B") == 0);
    CHECK(strcmp(S2_GetHIDButtonName(S2_HID_BUTTON_C), "C") == 0);
    CHECK(S2_GetHIDButtonName(S2_HID_NUM_BUTTONS) == NULL);
    CHECK(state.axes[0] == 0x123 && state.axes[1] == 0x456);
    CHECK(state.axes[2] == 0xFFF && state.axes[3] == 0x800);

    for (size_t i = 0; i < count; i++) {
        format_commands += S2_IsReportFormatCommand(commands[i].data);
    }
    CHECK(format_commands == 1);

    count = S2_GetUSBQuietSequence(&commands);
    CHECK(count == 1);
    CHECK(S2_IsReportFormatCommand(commands[0].data));
    CHECK(commands[0].data[S2_REPORT_FORMAT_OFFSET] == S2_REPORT_FORMAT_FULL);
    CHECK(S2_GetCommandSize(commands[0].data) == commands[0].size);
}

static void TestStickCalibration(void)
{
    /* neutral (2048, 2000), max (1500, 1400), min (1600, 1300) */
    const uint8_t packed[9] = {
        0x00, 0x08, 0x7D,
        0xDC, 0x85, 0x57,
        0x40, 0x46, 0x51
    };
    S2_StickCalibration cal;

    S2_ParseStickCalibration(&cal, packed);
    CHECK(cal.x.neutral == 2048);
    CHECK(cal.y.neutral == 2000);
    CHECK(cal.x.max == 1500);
    CHECK(cal.y.max == 1400);
    CHECK(cal.x.min == 1600);
    CHECK(cal.y.min == 1300);

    CHECK(S2_MapStickAxis(&cal.x, 2048, false) == 0);
    CHECK(S2_MapStickAxis(&cal.x, 2048 + 1500, false) == 32767);
    CHECK(S2_MapStickAxis(&cal.x, 2048 - 1600, false) == -32767);
    CHECK(S2_MapStickAxis(&cal.x, 4095, false) == 32767);   /* clamped */
    CHECK(S2_MapStickAxis(NULL, 0, false) == -32768);
    CHECK(S2_MapStickAxis(NULL, 2048, false) == 0 || S2_MapStickAxis(NULL, 2048, false) == -1);
}

static void TestDescriptorVerdict(void)
{
    /* Gamepad: 16 buttons + X/Y, report ID 1 */
    static const uint8_t gamepad[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x26, 0xFF, 0x0F,
        0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
        0xC0
    };
    /* Vendor page 0xFF00, 63 opaque bytes */
    static const uint8_t vendor[] = {
        0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x09,
        0x09, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x02,
        0xC0
    };
    PC2_DescriptorSummary summary;

    CHECK(PC2_ParseReportDescriptor(gamepad, sizeof(gamepad), &summary, NULL) == PC2_DESCRIPTOR_STANDARD);
    CHECK(summary.standard_inputs == 2 && summary.report_ids == 1);
    CHECK(summary.num_reports == 1 && summary.reports[0].id == 1 && summary.reports[0].input_bits == 48);
    CHECK(summary.top_usage_page == 0x01 && summary.top_usage == 0x05);

    CHECK(PC2_ParseReportDescriptor(vendor, sizeof(vendor), &summary, NULL) == PC2_DESCRIPTOR_VENDOR);
    CHECK(summary.top_usage_page == 0xFF00);
}

int main(void)
{
    TestInitSequenceFraming();
    TestReplyMatching();
    TestFlashRead();
    TestParseProReport();
    TestParseHIDReport();
    TestStickCalibration();
    TestDescriptorVerdict();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("s2_proto: all tests passed\n");
    return 0;
}
