/*
  s2_proto: Switch 2 controller protocol core.
*/
#include "s2_proto.h"

#include <string.h>

/* USB init sequence, from SDL 3.4 HIDAPI_DriverSwitch2_InitUSB(), except
   for the report format: SDL selects 0x05 (vendor report 0x05, which its own
   driver parses), but only report 0x09 declares standard HID usages that
   generic HID drivers and SDL's IOKit backend can use. */
static const uint8_t s2_init_unknown_07[] = {
    0x07, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_feature_mask[] = {
    0x0c, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00,
    0x27, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_unknown_11[] = {
    0x11, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_rumble_data[] = {
    0x0a, 0x91, 0x00, 0x08, 0x00, 0x14, 0x00, 0x00,
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_feature_enable[] = {
    0x0c, 0x91, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
    0x27, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_unknown_01_0c[] = {
    0x01, 0x91, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_enable_rumble[] = {
    0x01, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_grip_buttons[] = {
    0x08, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_report_format[] = {
    0x03, 0x91, 0x00, 0x0a, 0x00, 0x04, 0x00, 0x00,
    S2_REPORT_FORMAT_HID, 0x00, 0x00, 0x00
};
static const uint8_t s2_init_start_output[] = {
    0x03, 0x91, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x00,
    0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static const uint8_t s2_quiet_report_format[] = {
    0x03, 0x91, 0x00, 0x0a, 0x00, 0x04, 0x00, 0x00,
    S2_REPORT_FORMAT_FULL, 0x00, 0x00, 0x00
};

#define S2_COMMAND(name, array) { name, array, sizeof(array) }

static const S2_Command s2_usb_init_sequence[] = {
    S2_COMMAND("unknown 07/01", s2_init_unknown_07),
    S2_COMMAND("set feature mask", s2_init_feature_mask),
    S2_COMMAND("unknown 11/01", s2_init_unknown_11),
    S2_COMMAND("set rumble data", s2_init_rumble_data),
    S2_COMMAND("enable features", s2_init_feature_enable),
    S2_COMMAND("unknown 01/0c", s2_init_unknown_01_0c),
    S2_COMMAND("enable rumble", s2_init_enable_rumble),
    S2_COMMAND("enable grip buttons", s2_init_grip_buttons),
    S2_COMMAND("set report format", s2_init_report_format),
    S2_COMMAND("start output", s2_init_start_output),
};

static const S2_Command s2_usb_quiet_sequence[] = {
    S2_COMMAND("vendor report format", s2_quiet_report_format),
};

bool S2_IsSupportedProduct(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != S2_VENDOR_NINTENDO) {
        return false;
    }
    switch (product_id) {
    case S2_PRODUCT_PRO:
        return true;
    default:
        /* Joy-Con 2 and GameCube are known but untested */
        return false;
    }
}

const char *S2_GetProductName(uint16_t product_id)
{
    switch (product_id) {
    case S2_PRODUCT_JOYCON_LEFT:
        return "Joy-Con 2 (L)";
    case S2_PRODUCT_JOYCON_RIGHT:
        return "Joy-Con 2 (R)";
    case S2_PRODUCT_PRO:
        return "Switch 2 Pro Controller";
    case S2_PRODUCT_GAMECUBE:
        return "NSO GameCube Controller";
    default:
        return "Unknown Switch 2 controller";
    }
}

size_t S2_GetUSBInitSequence(const S2_Command **commands)
{
    *commands = s2_usb_init_sequence;
    return sizeof(s2_usb_init_sequence) / sizeof(s2_usb_init_sequence[0]);
}

size_t S2_GetUSBQuietSequence(const S2_Command **commands)
{
    *commands = s2_usb_quiet_sequence;
    return sizeof(s2_usb_quiet_sequence) / sizeof(s2_usb_quiet_sequence[0]);
}

size_t S2_GetCommandSize(const uint8_t *data)
{
    return S2_CMD_HEADER_SIZE + data[5];
}

bool S2_IsReportFormatCommand(const uint8_t *data)
{
    return data[0] == 0x03 && data[3] == 0x0a;
}

bool S2_IsReplyTo(const uint8_t *command, const uint8_t *reply, size_t reply_size)
{
    return reply_size >= S2_CMD_HEADER_SIZE && reply[0] == command[0] && reply[3] == command[3];
}

size_t S2_BuildFlashRead(uint8_t *out, uint32_t address)
{
    static const uint8_t flash_read[] = {
        0x02, 0x91, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    memcpy(out, flash_read, sizeof(flash_read));
    out[12] = (uint8_t)address;
    out[13] = (uint8_t)(address >> 8);
    out[14] = (uint8_t)(address >> 16);
    out[15] = (uint8_t)(address >> 24);
    return sizeof(flash_read);
}

void S2_ParseStickCalibration(S2_StickCalibration *cal, const uint8_t *data)
{
    cal->x.neutral = (uint16_t)(data[0] | ((data[1] & 0x0F) << 8));
    cal->y.neutral = (uint16_t)((data[1] >> 4) | (data[2] << 4));
    cal->x.max = (uint16_t)(data[3] | ((data[4] & 0x0F) << 8));
    cal->y.max = (uint16_t)((data[4] >> 4) | (data[5] << 4));
    cal->x.min = (uint16_t)(data[6] | ((data[7] & 0x0F) << 8));
    cal->y.min = (uint16_t)((data[7] >> 4) | (data[8] << 4));
}

int16_t S2_MapStickAxis(const S2_AxisCalibration *cal, uint16_t raw, bool invert)
{
    float value = (float)raw;
    float mapped;

    if (cal && cal->neutral && cal->min && cal->max) {
        value -= cal->neutral;
        value /= (value < 0.0f) ? cal->min : cal->max;
        mapped = value * 32767.0f;
    } else {
        mapped = (value / 4096.0f) * 65535.0f - 32768.0f;
    }
    if (mapped < -32768.0f) {
        mapped = -32768.0f;
    } else if (mapped > 32767.0f) {
        mapped = 32767.0f;
    }
    if (invert) {
        return (int16_t)~(int16_t)mapped;
    }
    return (int16_t)mapped;
}

bool S2_ParseProReport(S2_ProState *state, const uint8_t *report, size_t size)
{
    if (size < S2_INPUT_REPORT_MIN_SIZE) {
        return false;
    }

    state->buttons = (uint32_t)report[5] |
                     ((uint32_t)report[6] << 8) |
                     ((uint32_t)report[7] << 16) |
                     ((uint32_t)report[8] << 24);
    state->left_x = (uint16_t)(report[11] | ((report[12] & 0x0F) << 8));
    state->left_y = (uint16_t)((report[12] >> 4) | (report[13] << 4));
    state->right_x = (uint16_t)(report[14] | ((report[15] & 0x0F) << 8));
    state->right_y = (uint16_t)((report[15] >> 4) | (report[16] << 4));
    return true;
}

bool S2_ParseHIDReport(S2_HIDState *state, const uint8_t *report, size_t size)
{
    if (size < 12 || report[0] != S2_HID_REPORT_ID) {
        return false;
    }

    state->buttons = ((uint32_t)report[3] |
                      ((uint32_t)report[4] << 8) |
                      ((uint32_t)report[5] << 16)) & ((1u << S2_HID_NUM_BUTTONS) - 1);
    state->axes[0] = (uint16_t)(report[6] | ((report[7] & 0x0F) << 8));
    state->axes[1] = (uint16_t)((report[7] >> 4) | (report[8] << 4));
    state->axes[2] = (uint16_t)(report[9] | ((report[10] & 0x0F) << 8));
    state->axes[3] = (uint16_t)((report[10] >> 4) | (report[11] << 4));
    return true;
}

const char *S2_GetHIDButtonName(int button)
{
    static const char *names[S2_HID_NUM_BUTTONS] = {
        "B", "A", "Y", "X", "R", "ZR", "+", "RS", "Down", "Right", "Left",
        "Up", "L", "ZL", "-", "LS", "Home", "Capture", "GR", "GL", "C"
    };

    if (button < 0 || button >= S2_HID_NUM_BUTTONS) {
        return NULL;
    }
    return names[button];
}

const char *S2_GetProButtonName(uint32_t button)
{
    switch (button) {
    case S2_PRO_BUTTON_Y: return "Y";
    case S2_PRO_BUTTON_X: return "X";
    case S2_PRO_BUTTON_B: return "B";
    case S2_PRO_BUTTON_A: return "A";
    case S2_PRO_BUTTON_R: return "R";
    case S2_PRO_BUTTON_ZR: return "ZR";
    case S2_PRO_BUTTON_MINUS: return "-";
    case S2_PRO_BUTTON_PLUS: return "+";
    case S2_PRO_BUTTON_RSTICK: return "RS";
    case S2_PRO_BUTTON_LSTICK: return "LS";
    case S2_PRO_BUTTON_HOME: return "Home";
    case S2_PRO_BUTTON_CAPTURE: return "Capture";
    case S2_PRO_BUTTON_C: return "C";
    case S2_PRO_BUTTON_DOWN: return "Down";
    case S2_PRO_BUTTON_UP: return "Up";
    case S2_PRO_BUTTON_RIGHT: return "Right";
    case S2_PRO_BUTTON_LEFT: return "Left";
    case S2_PRO_BUTTON_L: return "L";
    case S2_PRO_BUTTON_ZL: return "ZL";
    case S2_PRO_BUTTON_GR: return "GR";
    case S2_PRO_BUTTON_GL: return "GL";
    default: return NULL;
    }
}
