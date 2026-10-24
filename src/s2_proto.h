/*
  s2_proto: Switch 2 controller protocol core.

  Transport-agnostic: no I/O, no allocation. Callers own all buffers.
  Shared by procon2-usb (wired) and s2ble (Bluetooth), written so it can be
  lifted into SDL later.

  Sources: SDL 3.4 src/joystick/hidapi/SDL_hidapi_switch2.c,
  novakpetya/linux-switch2, HandheldLegend/procon2tool.
*/
#ifndef S2_PROTO_H
#define S2_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2_VENDOR_NINTENDO          0x057E

#define S2_PRODUCT_JOYCON_LEFT      0x2066
#define S2_PRODUCT_JOYCON_RIGHT     0x2067
#define S2_PRODUCT_PRO              0x2069
#define S2_PRODUCT_GAMECUBE         0x2073

/* USB: the vendor-specific interface carrying bulk command traffic */
#define S2_USB_COMMAND_INTERFACE    1

/* Command framing: [cmd, 0x91, 0x00, subcmd, 0x00, payload_len, 0x00, 0x00, payload...] */
#define S2_CMD_HEADER_SIZE          8
#define S2_CMD_MAX_SIZE             64
#define S2_REPLY_MAX_SIZE           64

/* Input reports are at least this long, including the report ID at [0] */
#define S2_INPUT_REPORT_MIN_SIZE    64

/* Payload of the "set report format" command (03/0A). Each format makes the
   controller send a matching HID input report ID. */
#define S2_REPORT_FORMAT_FULL       0x05    /* report 0x05: vendor layout, S2_ParseProReport() */
#define S2_REPORT_FORMAT_HID        0x09    /* report 0x09: standard buttons/axes, S2_ParseHIDReport() */
#define S2_REPORT_FORMAT_OFFSET     8

typedef struct S2_Command
{
    const char *name;
    const uint8_t *data;
    size_t size;
} S2_Command;

/* Product IDs this code knows about. Only S2_PRODUCT_PRO is tested. */
bool S2_IsSupportedProduct(uint16_t vendor_id, uint16_t product_id);
const char *S2_GetProductName(uint16_t product_id);

/* The USB init sequence, in order. Returns the number of commands. */
size_t S2_GetUSBInitSequence(const S2_Command **commands);

/* Switches to report format 0x05, whose fields are all vendor-defined, so the
   HID system stops seeing gamepad button/axis changes. Used before system
   sleep: stick noise from the 250 Hz stream otherwise counts as user activity
   and makes macOS abort sleep. Undone by the init sequence. (Clearing the
   feature mask or sending "start output" 00 doesn't stop the stream.) */
size_t S2_GetUSBQuietSequence(const S2_Command **commands);

/* Total on-wire size of a framed command (header + payload_len) */
size_t S2_GetCommandSize(const uint8_t *data);

/* True for the "set report format" command, whose format byte is at
   S2_REPORT_FORMAT_OFFSET */
bool S2_IsReportFormatCommand(const uint8_t *data);

/* Replies echo the command and subcommand bytes; [1] is a status byte,
   0x01 on success (0x04 seen for commands the Pro Controller rejects) */
#define S2_REPLY_STATUS_OK          0x01
bool S2_IsReplyTo(const uint8_t *command, const uint8_t *reply, size_t reply_size);

/* Fill `out` (>= 16 bytes) with a flash read command for one 0x40 byte block.
   The block data starts at offset 0x10 of the reply. Returns the size. */
size_t S2_BuildFlashRead(uint8_t *out, uint32_t address);

#define S2_FLASH_SERIAL             0x13000
#define S2_FLASH_LEFT_STICK_CAL     0x13080
#define S2_FLASH_RIGHT_STICK_CAL    0x130C0
#define S2_FLASH_USER_LEFT_STICK    0x1FC040
#define S2_FLASH_USER_RIGHT_STICK   0x1FC080
#define S2_FLASH_REPLY_DATA_OFFSET  0x10
#define S2_FLASH_BLOCK_SIZE         0x40

/* Raw 12-bit stick values, 0..4095, centered near 2048 */
typedef struct S2_AxisCalibration
{
    uint16_t neutral;
    uint16_t max;   /* distance from neutral to the positive extreme */
    uint16_t min;   /* distance from neutral to the negative extreme */
} S2_AxisCalibration;

typedef struct S2_StickCalibration
{
    S2_AxisCalibration x;
    S2_AxisCalibration y;
} S2_StickCalibration;

/* Parse 9 packed bytes of stick calibration (factory: block + 0x28,
   user: block + 2 after the 0xB2 0xA1 magic) */
void S2_ParseStickCalibration(S2_StickCalibration *cal, const uint8_t *data);

/* Map a raw 12-bit axis value to -32768..32767. `cal` may be NULL. */
int16_t S2_MapStickAxis(const S2_AxisCalibration *cal, uint16_t raw, bool invert);

/* Pro Controller button bits, packed as in report bytes 5..8 (little-endian) */
typedef enum S2_ProButton
{
    S2_PRO_BUTTON_Y         = 1u << 0,
    S2_PRO_BUTTON_X         = 1u << 1,
    S2_PRO_BUTTON_B         = 1u << 2,
    S2_PRO_BUTTON_A         = 1u << 3,
    S2_PRO_BUTTON_R         = 1u << 6,
    S2_PRO_BUTTON_ZR        = 1u << 7,
    S2_PRO_BUTTON_MINUS     = 1u << 8,
    S2_PRO_BUTTON_PLUS      = 1u << 9,
    S2_PRO_BUTTON_RSTICK    = 1u << 10,
    S2_PRO_BUTTON_LSTICK    = 1u << 11,
    S2_PRO_BUTTON_HOME      = 1u << 12,
    S2_PRO_BUTTON_CAPTURE   = 1u << 13,
    S2_PRO_BUTTON_C         = 1u << 14,
    S2_PRO_BUTTON_DOWN      = 1u << 16,
    S2_PRO_BUTTON_UP        = 1u << 17,
    S2_PRO_BUTTON_RIGHT     = 1u << 18,
    S2_PRO_BUTTON_LEFT      = 1u << 19,
    S2_PRO_BUTTON_L         = 1u << 22,
    S2_PRO_BUTTON_ZL        = 1u << 23,
    S2_PRO_BUTTON_GR        = 1u << 24,
    S2_PRO_BUTTON_GL        = 1u << 25
} S2_ProButton;

typedef struct S2_ProState
{
    uint32_t buttons;       /* S2_ProButton bits */
    uint16_t left_x;        /* raw 12-bit */
    uint16_t left_y;
    uint16_t right_x;
    uint16_t right_y;
} S2_ProState;

/* Parse a Pro Controller input report. `report` starts with the report ID,
   as hidapi and IOHIDDevice deliver it. Returns false if it's too short. */
bool S2_ParseProReport(S2_ProState *state, const uint8_t *report, size_t size);

/* Report 0x09 as declared by the HID descriptor: 2 vendor bytes, 21 buttons,
   3 bits padding, X/Y/Rx/Rz at 12 bits each, then vendor data */
#define S2_HID_REPORT_ID            0x09
#define S2_HID_NUM_BUTTONS          21
#define S2_HID_NUM_AXES             4

/* Report 0x09 button numbers (0-based HID button index), learned on hardware */
typedef enum S2_HIDButton
{
    S2_HID_BUTTON_B = 0,
    S2_HID_BUTTON_A,
    S2_HID_BUTTON_Y,
    S2_HID_BUTTON_X,
    S2_HID_BUTTON_R,
    S2_HID_BUTTON_ZR,
    S2_HID_BUTTON_PLUS,
    S2_HID_BUTTON_RSTICK,
    S2_HID_BUTTON_DOWN,
    S2_HID_BUTTON_RIGHT,
    S2_HID_BUTTON_LEFT,
    S2_HID_BUTTON_UP,
    S2_HID_BUTTON_L,
    S2_HID_BUTTON_ZL,
    S2_HID_BUTTON_MINUS,
    S2_HID_BUTTON_LSTICK,
    S2_HID_BUTTON_HOME,
    S2_HID_BUTTON_CAPTURE,
    S2_HID_BUTTON_GR,
    S2_HID_BUTTON_GL,
    S2_HID_BUTTON_C
} S2_HIDButton;

/* Axes: X/Y left stick, Rx/Rz right stick; Y and Rz increase upwards */
#define S2_HID_AXIS_LEFT_X          0
#define S2_HID_AXIS_LEFT_Y          1
#define S2_HID_AXIS_RIGHT_X         2
#define S2_HID_AXIS_RIGHT_Y         3

typedef struct S2_HIDState
{
    uint32_t buttons;               /* bit n = HID button n + 1 */
    uint16_t axes[S2_HID_NUM_AXES]; /* raw 12-bit X, Y, Rx, Rz */
} S2_HIDState;

/* Parse report 0x09, starting with the report ID. Returns false if it's
   another report or too short. */
bool S2_ParseHIDReport(S2_HIDState *state, const uint8_t *report, size_t size);

/* Name of an S2_HIDButton, or NULL */
const char *S2_GetHIDButtonName(int button);

/* Name of a single S2_ProButton bit, or NULL */
const char *S2_GetProButtonName(uint32_t button);

#ifdef __cplusplus
}
#endif

#endif /* S2_PROTO_H */
