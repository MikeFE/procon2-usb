/*
  procon2-usb: wired Switch 2 Pro Controller enabler for macOS.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc2_agent.h"
#include "pc2_hid.h"
#include "pc2_usb.h"
#include "s2_proto.h"

#define PC2_EXIT_OK         0
#define PC2_EXIT_USAGE      64
#define PC2_EXIT_NOT_FOUND  2
#define PC2_EXIT_OPEN       3
#define PC2_EXIT_COMMAND    4
#define PC2_EXIT_NO_INPUT   5

#define PC2_VERIFY_TIMEOUT_S    2.0

static void PrintUsage(FILE *out)
{
    fprintf(out,
            "usage: procon2-usb <command> [options]\n"
            "\n"
            "commands:\n"
            "  enable [-v] [--verify] [--format N]\n"
            "                           send the USB init sequence\n"
            "                           -v: print each command's reply\n"
            "                           --verify: wait for HID input afterwards\n"
            "                           --format: report format byte (default 0x09;\n"
            "                           0x05 is SDL's vendor layout)\n"
            "  monitor [--raw]          print live button and stick state\n"
            "                           --raw: print changed report bytes instead\n"
            "  learn                    guided button/stick mapping; prints an SDL\n"
            "                           mapping string for Moonlight\n"
            "  descriptor [--hex]       dump the HID report descriptor\n"
            "  quiet                    hide input from macOS (vendor report 0x05) until the next enable\n"
            "  sequence                 print the init sequence without sending it\n"
            "  send <hex bytes>         send one raw command and print the reply (developer)\n"
            "  agent [--sdl-mapping FILE]\n"
            "                           run in the background: enable on attach and wake;\n"
            "                           publish FILE as SDL_GAMECONTROLLERCONFIG_FILE\n"
            "                           (logs: log stream --predicate 'subsystem == \"" PC2_LOG_SUBSYSTEM "\"')\n");
}

static void PrintBytes(FILE *out, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        fprintf(out, "%s%02X", i ? " " : "", data[i]);
    }
}

/* `userdata` points to the sequence length */
static void PrintReply(void *userdata, size_t index, const S2_Command *command,
                       const uint8_t *reply, uint32_t reply_size)
{
    size_t count = *(const size_t *)userdata;

    printf("  [%zu/%zu] %-20s -> ", index + 1, count, command->name);
    if (reply) {
        printf("%s, %u bytes: ", (reply[1] == S2_REPLY_STATUS_OK) ? "ok" : "rejected", reply_size);
        PrintBytes(stdout, reply, reply_size);
        printf("\n");
    } else {
        printf("no reply\n");
    }
}

static int ExitCodeForStatus(PC2_Status status)
{
    switch (status) {
    case PC2_OK:
        return PC2_EXIT_OK;
    case PC2_ERR_NOT_FOUND:
        return PC2_EXIT_NOT_FOUND;
    case PC2_ERR_COMMAND:
        return PC2_EXIT_COMMAND;
    default:
        return PC2_EXIT_OPEN;
    }
}

static void PrintError(const PC2_USBContext *ctx, PC2_Status status)
{
    const S2_Command *commands;
    size_t count = S2_GetUSBInitSequence(&commands);

    fprintf(stderr, "error: %s", PC2_GetStatusString(status));
    if (ctx->last_error != kIOReturnSuccess) {
        fprintf(stderr, " (IOReturn 0x%08X)", ctx->last_error);
    }
    fprintf(stderr, "\n");

    switch (status) {
    case PC2_ERR_NOT_FOUND:
        fprintf(stderr, "  Is the controller connected with a USB-C data cable?\n");
        break;
    case PC2_ERR_EXCLUSIVE:
        fprintf(stderr, "  Another program (Steam, a game, another procon2 tool) may have it open.\n"
                        "  Quit it, or retry with sudo.\n");
        break;
    case PC2_ERR_NOT_PERMITTED:
    case PC2_ERR_OPEN:
    case PC2_ERR_PLUGIN:
        fprintf(stderr, "  Retry with sudo.\n");
        break;
    case PC2_ERR_COMMAND:
        if (ctx->failed_command >= 0 && (size_t)ctx->failed_command < count) {
            fprintf(stderr, "  Failed at command %d/%zu (%s).\n", ctx->failed_command + 1, count,
                    commands[ctx->failed_command].name);
        }
        fprintf(stderr, "  Unplug and reconnect the controller, then retry.\n");
        break;
    default:
        break;
    }
}

static int CommandEnable(int argc, char **argv)
{
    PC2_USBContext ctx;
    PC2_Status status;
    io_service_t service;
    uint16_t product_id = 0;
    bool verbose = false;
    bool verify = false;
    unsigned long report_format = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--verify") == 0) {
            verify = true;
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            char *end;
            report_format = strtoul(argv[++i], &end, 0);
            if (*end || report_format == 0 || report_format > 0xFF) {
                fprintf(stderr, "error: --format takes a byte, e.g. 0x09\n");
                return PC2_EXIT_USAGE;
            }
        } else {
            PrintUsage(stderr);
            return PC2_EXIT_USAGE;
        }
    }

    PC2_USBInit(&ctx);
    ctx.report_format = (uint8_t)report_format;
    service = PC2_USBFindCommandInterface(&product_id);
    if (service == IO_OBJECT_NULL) {
        PrintError(&ctx, PC2_ERR_NOT_FOUND);
        return PC2_EXIT_NOT_FOUND;
    }

    status = PC2_USBOpen(&ctx, service);
    IOObjectRelease(service);
    if (status == PC2_OK) {
        printf("Found %s (%04X:%04X) at location 0x%08llX\n", S2_GetProductName(product_id),
               S2_VENDOR_NINTENDO, product_id, (unsigned long long)ctx.location_id);
        if (verbose) {
            printf("  bulk OUT pipe %u, bulk IN pipe %u\n", ctx.pipe_out, ctx.pipe_in);
        }
        if (ctx.report_format) {
            printf("  report format override: 0x%02X\n", ctx.report_format);
        }
        const S2_Command *commands;
        size_t count = S2_GetUSBInitSequence(&commands);
        status = PC2_USBSendInit(&ctx, verbose ? PrintReply : NULL, &count);
    }
    if (status != PC2_OK) {
        PrintError(&ctx, status);
        PC2_USBClose(&ctx);
        return ExitCodeForStatus(status);
    }
    PC2_USBClose(&ctx);
    printf("Init sequence sent.\n");

    if (verify) {
        if (!PC2_HIDWaitForReport(PC2_VERIFY_TIMEOUT_S)) {
            fprintf(stderr, "error: no HID input reports within %.0f s after init\n", PC2_VERIFY_TIMEOUT_S);
            return PC2_EXIT_NO_INPUT;
        }
        printf("HID input reports are flowing.\n");
    }
    return PC2_EXIT_OK;
}

static int CommandQuiet(void)
{
    PC2_USBContext ctx;
    PC2_Status status;
    io_service_t service;
    const S2_Command *commands;
    size_t count = S2_GetUSBQuietSequence(&commands);

    PC2_USBInit(&ctx);
    service = PC2_USBFindCommandInterface(NULL);
    if (service == IO_OBJECT_NULL) {
        PrintError(&ctx, PC2_ERR_NOT_FOUND);
        return PC2_EXIT_NOT_FOUND;
    }
    status = PC2_USBOpen(&ctx, service);
    IOObjectRelease(service);
    if (status == PC2_OK) {
        status = PC2_USBSendSequence(&ctx, commands, count, PrintReply, &count);
    }
    if (status != PC2_OK) {
        PrintError(&ctx, status);
    }
    PC2_USBClose(&ctx);
    return ExitCodeForStatus(status);
}

/* Developer tool: send one raw framed command, e.g. `send 03 91 00 0d 00 08 00 00 ...` */
static int CommandSend(int argc, char **argv)
{
    PC2_USBContext ctx;
    PC2_Status status;
    io_service_t service;
    uint8_t data[S2_CMD_MAX_SIZE];
    S2_Command command = { "raw command", data, 0 };
    size_t count = 1;

    if (argc < S2_CMD_HEADER_SIZE || argc > S2_CMD_MAX_SIZE) {
        fprintf(stderr, "error: send takes %d to %d hex bytes\n", S2_CMD_HEADER_SIZE, S2_CMD_MAX_SIZE);
        return PC2_EXIT_USAGE;
    }
    for (int i = 0; i < argc; i++) {
        char *end;
        unsigned long value = strtoul(argv[i], &end, 16);
        if (*end || value > 0xFF) {
            fprintf(stderr, "error: not a hex byte: %s\n", argv[i]);
            return PC2_EXIT_USAGE;
        }
        data[i] = (uint8_t)value;
    }
    command.size = (size_t)argc;
    if (S2_GetCommandSize(data) != command.size) {
        fprintf(stderr, "warning: length byte says %zu bytes, sending %zu\n", S2_GetCommandSize(data), command.size);
    }

    PC2_USBInit(&ctx);
    service = PC2_USBFindCommandInterface(NULL);
    if (service == IO_OBJECT_NULL) {
        PrintError(&ctx, PC2_ERR_NOT_FOUND);
        return PC2_EXIT_NOT_FOUND;
    }
    status = PC2_USBOpen(&ctx, service);
    IOObjectRelease(service);
    if (status == PC2_OK) {
        status = PC2_USBSendSequence(&ctx, &command, 1, PrintReply, &count);
    }
    if (status != PC2_OK) {
        PrintError(&ctx, status);
    }
    PC2_USBClose(&ctx);
    return ExitCodeForStatus(status);
}

static int CommandMonitor(int argc, char **argv)
{
    bool raw = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            raw = true;
        } else {
            PrintUsage(stderr);
            return PC2_EXIT_USAGE;
        }
    }
    return PC2_HIDMonitor(raw);
}

static int CommandDescriptor(int argc, char **argv)
{
    bool raw_only = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--hex") == 0) {
            raw_only = true;
        } else {
            PrintUsage(stderr);
            return PC2_EXIT_USAGE;
        }
    }
    if (PC2_HIDDumpDescriptors(stdout, raw_only) == 0) {
        fprintf(stderr, "error: no HID interface found for %04X:%04X\n", S2_VENDOR_NINTENDO, S2_PRODUCT_PRO);
        return PC2_EXIT_NOT_FOUND;
    }
    return PC2_EXIT_OK;
}

static int CommandAgent(int argc, char **argv)
{
    const char *mapping_path = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--sdl-mapping") == 0 && i + 1 < argc) {
            mapping_path = argv[++i];
        } else {
            PrintUsage(stderr);
            return PC2_EXIT_USAGE;
        }
    }
    return PC2_AgentRun(mapping_path);
}

static int CommandSequence(void)
{
    const S2_Command *commands;
    size_t count = S2_GetUSBInitSequence(&commands);

    for (size_t i = 0; i < count; i++) {
        printf("%2zu %-20s ", i + 1, commands[i].name);
        PrintBytes(stdout, commands[i].data, commands[i].size);
        printf("\n");
    }
    return PC2_EXIT_OK;
}

int main(int argc, char **argv)
{
    const char *command;

    if (argc < 2) {
        PrintUsage(stderr);
        return PC2_EXIT_USAGE;
    }
    command = argv[1];
    argc -= 2;
    argv += 2;

    if (strcmp(command, "enable") == 0) {
        return CommandEnable(argc, argv);
    } else if (strcmp(command, "monitor") == 0) {
        return CommandMonitor(argc, argv);
    } else if (strcmp(command, "learn") == 0) {
        return PC2_HIDLearn();
    } else if (strcmp(command, "descriptor") == 0) {
        return CommandDescriptor(argc, argv);
    } else if (strcmp(command, "send") == 0) {
        return CommandSend(argc, argv);
    } else if (strcmp(command, "quiet") == 0) {
        return CommandQuiet();
    } else if (strcmp(command, "agent") == 0) {
        return CommandAgent(argc, argv);
    } else if (strcmp(command, "sequence") == 0) {
        return CommandSequence();
    } else if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0 || strcmp(command, "help") == 0) {
        PrintUsage(stdout);
        return PC2_EXIT_OK;
    }
    PrintUsage(stderr);
    return PC2_EXIT_USAGE;
}
