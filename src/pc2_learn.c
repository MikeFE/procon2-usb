/*
  pc2_learn: guided mapping of report 0x09 buttons and axes to physical
  controls, ending with an SDL game controller mapping string.
*/
#include "pc2_hid.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <string.h>

#include "s2_proto.h"

#define PC2_LEARN_STEP_TIMEOUT_S    10.0
#define PC2_LEARN_ATTACH_TIMEOUT_S  3.0
#define PC2_LEARN_TICK_S            0.25
#define PC2_AXIS_PUSH_THRESHOLD     1200    /* raw units from rest, of 4095 */
#define PC2_AXIS_REST_THRESHOLD     400
#define PC2_LEARN_BUFFER_SIZE       512

typedef enum PC2_ControlKind
{
    PC2_CONTROL_BUTTON,
    PC2_CONTROL_AXIS_RIGHT,     /* SDL: right is positive */
    PC2_CONTROL_AXIS_UP         /* SDL: up is negative */
} PC2_ControlKind;

typedef struct PC2_Control
{
    const char *prompt;
    const char *label;
    const char *sdl_name;
    PC2_ControlKind kind;
} PC2_Control;

/* SDL mappings are positional: "a" is the bottom face button, which is B on
   Nintendo controllers. */
static const PC2_Control pc2_controls[] = {
    { "Press A", "A", "b", PC2_CONTROL_BUTTON },
    { "Press B", "B", "a", PC2_CONTROL_BUTTON },
    { "Press X", "X", "y", PC2_CONTROL_BUTTON },
    { "Press Y", "Y", "x", PC2_CONTROL_BUTTON },
    { "Press L", "L", "leftshoulder", PC2_CONTROL_BUTTON },
    { "Press R", "R", "rightshoulder", PC2_CONTROL_BUTTON },
    { "Press ZL", "ZL", "lefttrigger", PC2_CONTROL_BUTTON },
    { "Press ZR", "ZR", "righttrigger", PC2_CONTROL_BUTTON },
    { "Press - (minus)", "-", "back", PC2_CONTROL_BUTTON },
    { "Press + (plus)", "+", "start", PC2_CONTROL_BUTTON },
    { "Press Home", "Home", "guide", PC2_CONTROL_BUTTON },
    { "Press Capture", "Capture", "misc1", PC2_CONTROL_BUTTON },
    { "Press C", "C", "misc2", PC2_CONTROL_BUTTON },
    { "Click the LEFT stick in", "L-stick click", "leftstick", PC2_CONTROL_BUTTON },
    { "Click the RIGHT stick in", "R-stick click", "rightstick", PC2_CONTROL_BUTTON },
    { "Press D-pad UP", "D-pad up", "dpup", PC2_CONTROL_BUTTON },
    { "Press D-pad DOWN", "D-pad down", "dpdown", PC2_CONTROL_BUTTON },
    { "Press D-pad LEFT", "D-pad left", "dpleft", PC2_CONTROL_BUTTON },
    { "Press D-pad RIGHT", "D-pad right", "dpright", PC2_CONTROL_BUTTON },
    { "Press GR (back, right)", "GR", "paddle1", PC2_CONTROL_BUTTON },
    { "Press GL (back, left)", "GL", "paddle2", PC2_CONTROL_BUTTON },
    { "Push the LEFT stick fully RIGHT, then let go", "Left stick X", "leftx", PC2_CONTROL_AXIS_RIGHT },
    { "Push the LEFT stick fully UP, then let go", "Left stick Y", "lefty", PC2_CONTROL_AXIS_UP },
    { "Push the RIGHT stick fully RIGHT, then let go", "Right stick X", "rightx", PC2_CONTROL_AXIS_RIGHT },
    { "Push the RIGHT stick fully UP, then let go", "Right stick Y", "righty", PC2_CONTROL_AXIS_UP },
};

#define PC2_NUM_CONTROLS (sizeof(pc2_controls) / sizeof(pc2_controls[0]))

static const char *pc2_axis_names[S2_HID_NUM_AXES] = { "X", "Y", "Rx", "Rz" };

typedef struct PC2_ControlResult
{
    int index;          /* 0-based button or axis index, -1 if skipped */
    bool inverted;
} PC2_ControlResult;

typedef struct PC2_LearnContext
{
    IOHIDDeviceRef device;
    bool done;
    bool failed;
    bool have_rest;
    uint16_t rest[S2_HID_NUM_AXES];
    size_t step;
    bool waiting_release;
    int candidate;
    bool candidate_increased;
    CFAbsoluteTime start_time;
    CFAbsoluteTime step_time;
    PC2_ControlResult results[PC2_NUM_CONTROLS];
    uint8_t buffer[PC2_LEARN_BUFFER_SIZE];
} PC2_LearnContext;

static void PC2_Prompt(PC2_LearnContext *ctx)
{
    fprintf(stderr, "[%2zu/%zu] %s  (wait %.0f s to skip)\n", ctx->step + 1, PC2_NUM_CONTROLS,
            pc2_controls[ctx->step].prompt, PC2_LEARN_STEP_TIMEOUT_S);
}

static void PC2_Finish(PC2_LearnContext *ctx, bool failed)
{
    ctx->done = true;
    ctx->failed = failed;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static void PC2_Advance(PC2_LearnContext *ctx, int index, bool inverted)
{
    ctx->results[ctx->step].index = index;
    ctx->results[ctx->step].inverted = inverted;
    ctx->waiting_release = false;
    ctx->step++;
    ctx->step_time = CFAbsoluteTimeGetCurrent();
    if (ctx->step == PC2_NUM_CONTROLS) {
        PC2_Finish(ctx, false);
    } else {
        PC2_Prompt(ctx);
    }
}

/* Returns the label of an earlier control already using this index, or NULL */
static const char *PC2_FindAssigned(const PC2_LearnContext *ctx, bool axis, int index)
{
    for (size_t i = 0; i < ctx->step; i++) {
        bool is_axis = pc2_controls[i].kind != PC2_CONTROL_BUTTON;
        if (is_axis == axis && ctx->results[i].index == index) {
            return pc2_controls[i].label;
        }
    }
    return NULL;
}

static void PC2_LearnButton(PC2_LearnContext *ctx, const S2_HIDState *state)
{
    if (!ctx->waiting_release) {
        /* Exactly one button down */
        if (state->buttons && !(state->buttons & (state->buttons - 1))) {
            ctx->candidate = __builtin_ctz(state->buttons);
            ctx->waiting_release = true;
        }
        return;
    }
    if (state->buttons) {
        return;
    }

    const char *owner = PC2_FindAssigned(ctx, false, ctx->candidate);
    if (owner) {
        fprintf(stderr, "        b%d is already %s. Try again.\n", ctx->candidate + 1, owner);
        ctx->waiting_release = false;
        ctx->step_time = CFAbsoluteTimeGetCurrent();
        return;
    }
    fprintf(stderr, "        -> b%d\n", ctx->candidate + 1);
    PC2_Advance(ctx, ctx->candidate, false);
}

static void PC2_LearnAxis(PC2_LearnContext *ctx, const S2_HIDState *state)
{
    PC2_ControlKind kind = pc2_controls[ctx->step].kind;

    if (!ctx->waiting_release) {
        int best = -1;
        int best_delta = 0;

        for (int i = 0; i < S2_HID_NUM_AXES; i++) {
            int delta = (int)state->axes[i] - (int)ctx->rest[i];
            int magnitude = delta < 0 ? -delta : delta;
            if (magnitude > PC2_AXIS_PUSH_THRESHOLD && magnitude > (best_delta < 0 ? -best_delta : best_delta)) {
                best = i;
                best_delta = delta;
            }
        }
        if (best >= 0) {
            ctx->candidate = best;
            ctx->candidate_increased = best_delta > 0;
            ctx->waiting_release = true;
        }
        return;
    }

    for (int i = 0; i < S2_HID_NUM_AXES; i++) {
        int delta = (int)state->axes[i] - (int)ctx->rest[i];
        if (delta > PC2_AXIS_REST_THRESHOLD || delta < -PC2_AXIS_REST_THRESHOLD) {
            return;
        }
    }

    const char *owner = PC2_FindAssigned(ctx, true, ctx->candidate);
    if (owner) {
        fprintf(stderr, "        %s is already %s. Try again.\n", pc2_axis_names[ctx->candidate], owner);
        ctx->waiting_release = false;
        ctx->step_time = CFAbsoluteTimeGetCurrent();
        return;
    }

    /* Right should read positive and up negative; invert otherwise */
    bool inverted = (kind == PC2_CONTROL_AXIS_RIGHT) ? !ctx->candidate_increased : ctx->candidate_increased;
    fprintf(stderr, "        -> %s (%s)%s\n", pc2_axis_names[ctx->candidate],
            ctx->candidate_increased ? "increases" : "decreases", inverted ? ", inverted" : "");
    PC2_Advance(ctx, ctx->candidate, inverted);
}

static void PC2_LearnReportCallback(void *context, IOReturn result, void *sender, IOHIDReportType type,
                                    uint32_t report_id, uint8_t *report, CFIndex report_length)
{
    PC2_LearnContext *ctx = (PC2_LearnContext *)context;
    S2_HIDState state;

    (void)sender;
    (void)type;
    if (result != kIOReturnSuccess || ctx->done) {
        return;
    }
    if (report_id != S2_HID_REPORT_ID) {
        fprintf(stderr, "error: the controller is sending report 0x%02X, not 0x%02X.\n"
                        "  Run `procon2-usb enable` (without --format) first.\n",
                report_id, S2_HID_REPORT_ID);
        PC2_Finish(ctx, true);
        return;
    }
    if (!S2_ParseHIDReport(&state, report, (size_t)report_length)) {
        return;
    }

    if (!ctx->have_rest) {
        if (state.buttons) {
            return;
        }
        memcpy(ctx->rest, state.axes, sizeof(ctx->rest));
        ctx->have_rest = true;
        ctx->step_time = CFAbsoluteTimeGetCurrent();
        fprintf(stderr, "Press each control when asked, one at a time, then let go.\n");
        PC2_Prompt(ctx);
        return;
    }

    if (pc2_controls[ctx->step].kind == PC2_CONTROL_BUTTON) {
        PC2_LearnButton(ctx, &state);
    } else {
        PC2_LearnAxis(ctx, &state);
    }
}

static void PC2_LearnMatchedCallback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    PC2_LearnContext *ctx = (PC2_LearnContext *)context;

    (void)result;
    (void)sender;
    if (ctx->device) {
        return;
    }
    ctx->device = device;
    IOHIDDeviceRegisterInputReportCallback(device, ctx->buffer, sizeof(ctx->buffer),
                                           PC2_LearnReportCallback, ctx);
}

static void PC2_LearnTimerCallback(CFRunLoopTimerRef timer, void *info)
{
    PC2_LearnContext *ctx = (PC2_LearnContext *)info;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

    (void)timer;
    if (ctx->done) {
        return;
    }
    if (!ctx->have_rest) {
        if (now - ctx->start_time > PC2_LEARN_ATTACH_TIMEOUT_S) {
            fprintf(stderr, "error: no input from the controller. Is it plugged in, and did you run "
                            "`procon2-usb enable`?\n");
            PC2_Finish(ctx, true);
        }
        return;
    }
    if (!ctx->waiting_release && now - ctx->step_time > PC2_LEARN_STEP_TIMEOUT_S) {
        fprintf(stderr, "        (skipped)\n");
        PC2_Advance(ctx, -1, false);
    }
}

static void PC2_PrintResults(const PC2_LearnContext *ctx)
{
    char product[128];
    int vendor_id = PC2_GetDeviceInt(ctx->device, CFSTR(kIOHIDVendorIDKey));
    int product_id = PC2_GetDeviceInt(ctx->device, CFSTR(kIOHIDProductIDKey));
    int version = PC2_GetDeviceInt(ctx->device, CFSTR(kIOHIDVersionNumberKey));

    PC2_GetDeviceString(ctx->device, CFSTR(kIOHIDProductKey), product, sizeof(product));
    if (!product[0]) {
        strcpy(product, S2_GetProductName((uint16_t)product_id));
    }

    printf("\nControl         HID          SDL\n");
    for (size_t i = 0; i < PC2_NUM_CONTROLS; i++) {
        const PC2_Control *control = &pc2_controls[i];
        const PC2_ControlResult *r = &ctx->results[i];
        char hid[16];

        if (r->index < 0) {
            strcpy(hid, "-");
        } else if (control->kind == PC2_CONTROL_BUTTON) {
            snprintf(hid, sizeof(hid), "b%d", r->index + 1);
        } else {
            snprintf(hid, sizeof(hid), "%s%s", pc2_axis_names[r->index], r->inverted ? " (inv)" : "");
        }
        printf("%-15s %-12s %s\n", control->label, hid, control->sdl_name);
    }

    /* SDL GUID: bus, CRC (0 matches any name), vendor, product, version,
       each 16-bit little-endian followed by 16 zero bits */
    printf("\nSDL_GAMECONTROLLERCONFIG=\"03000000%02x%02x0000%02x%02x0000%02x%02x0000,%s,",
           vendor_id & 0xFF, (vendor_id >> 8) & 0xFF,
           product_id & 0xFF, (product_id >> 8) & 0xFF,
           version & 0xFF, (version >> 8) & 0xFF,
           product);
    for (size_t i = 0; i < PC2_NUM_CONTROLS; i++) {
        const PC2_ControlResult *r = &ctx->results[i];
        if (r->index < 0) {
            continue;
        }
        if (pc2_controls[i].kind == PC2_CONTROL_BUTTON) {
            printf("%s:b%d,", pc2_controls[i].sdl_name, r->index);
        } else {
            printf("%s:a%d%s,", pc2_controls[i].sdl_name, r->index, r->inverted ? "~" : "");
        }
    }
    printf("platform:Mac OS X,\"\n");
}

int PC2_HIDLearn(void)
{
    static PC2_LearnContext ctx;
    IOHIDManagerRef manager = PC2_HIDCreateManager();
    CFRunLoopTimerContext timer_context = { 0, &ctx, NULL, NULL, NULL };
    CFRunLoopTimerRef timer;
    IOReturn result;

    memset(&ctx, 0, sizeof(ctx));
    ctx.start_time = CFAbsoluteTimeGetCurrent();

    IOHIDManagerRegisterDeviceMatchingCallback(manager, PC2_LearnMatchedCallback, &ctx);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    result = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "error: couldn't open the HID manager (0x%08X)\n", result);
        CFRelease(manager);
        return 1;
    }

    timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + PC2_LEARN_TICK_S,
                                 PC2_LEARN_TICK_S, 0, 0, PC2_LearnTimerCallback, &timer_context);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    while (!ctx.done) {
        CFRunLoopRun();
    }

    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
    if (!ctx.failed) {
        PC2_PrintResults(&ctx);
    }
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    return ctx.failed ? 2 : 0;
}
