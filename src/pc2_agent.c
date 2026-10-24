/*
  pc2_agent: background enabler run by a LaunchAgent.
*/
#include "pc2_agent.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <limits.h>
#include <os/log.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pc2_hid.h"
#include "pc2_usb.h"
#include "s2_proto.h"

/* Attach and wake events arrive in bursts (USB re-enumeration, wake +
   attach); wait for things to settle, then init everything once. */
#define PC2_ATTACH_DELAY_S      0.5
#define PC2_WAKE_DELAY_S        2.0
#define PC2_RETRY_DELAY_S       1.0
#define PC2_MAX_ATTEMPTS        4
#define PC2_VERIFY_TIMEOUT_S    1.5
#define PC2_MAX_PRODUCTS        8
#define PC2_TIMER_IDLE          1.0e10

extern char **environ;

typedef struct PC2_Agent
{
    os_log_t log;
    IONotificationPortRef notify_port;
    io_iterator_t attach_iterators[PC2_MAX_PRODUCTS];
    io_iterator_t detach_iterators[PC2_MAX_PRODUCTS];
    size_t num_iterators;

    io_connect_t power_root;
    IONotificationPortRef power_port;
    io_object_t power_notifier;

    CFRunLoopTimerRef timer;
    int attempt;
} PC2_Agent;

static void PC2_Schedule(PC2_Agent *agent, double delay_s, const char *reason)
{
    os_log_info(agent->log, "%{public}s; enabling in %.1f s", reason, delay_s);
    agent->attempt = 0;
    CFRunLoopTimerSetNextFireDate(agent->timer, CFAbsoluteTimeGetCurrent() + delay_s);
}

/* Returns true if the sequence went through */
static bool PC2_SendToService(PC2_Agent *agent, io_service_t service, const S2_Command *commands, size_t count,
                              const char *what)
{
    PC2_USBContext ctx;
    PC2_Status status;

    PC2_USBInit(&ctx);
    status = PC2_USBOpen(&ctx, service);
    if (status == PC2_OK) {
        status = PC2_USBSendSequence(&ctx, commands, count, NULL, NULL);
    }

    if (status == PC2_OK) {
        os_log(agent->log, "Sent %{public}s to %{public}s at 0x%08llx", what, S2_GetProductName(ctx.product_id),
               (unsigned long long)ctx.location_id);
    } else if (status == PC2_ERR_COMMAND) {
        os_log_error(agent->log, "%{public}s command %d failed at 0x%08llx: IOReturn 0x%08x", what,
                     ctx.failed_command + 1, (unsigned long long)ctx.location_id, ctx.last_error);
    } else {
        os_log_error(agent->log, "Couldn't open controller at 0x%08llx: %{public}s (IOReturn 0x%08x)",
                     (unsigned long long)ctx.location_id, PC2_GetStatusString(status), ctx.last_error);
    }
    PC2_USBClose(&ctx);
    return status == PC2_OK;
}

/* Send a sequence to every connected controller. Returns how many were
   found; `failed` gets how many of those failed. */
static int PC2_SendToAll(PC2_Agent *agent, const S2_Command *commands, size_t count, const char *what, int *failed)
{
    const uint16_t *product_ids;
    size_t num_products = PC2_USBGetProductIDs(&product_ids);
    int found = 0;

    *failed = 0;
    for (size_t i = 0; i < num_products; i++) {
        CFMutableDictionaryRef matching = PC2_USBCreateMatching(product_ids[i]);
        io_iterator_t iterator;
        io_service_t service;

        if (!matching || IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != kIOReturnSuccess) {
            continue;
        }
        while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
            found++;
            if (!PC2_SendToService(agent, service, commands, count, what)) {
                (*failed)++;
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
    }
    return found;
}

static void PC2_EnableAll(PC2_Agent *agent)
{
    const S2_Command *commands;
    size_t count = S2_GetUSBInitSequence(&commands);
    int failed;
    int found = PC2_SendToAll(agent, commands, count, "init", &failed);

    if (found == 0) {
        os_log_info(agent->log, "No controller present");
        return;
    }
    if (failed == 0 && !PC2_HIDWaitForReport(PC2_VERIFY_TIMEOUT_S)) {
        os_log_error(agent->log, "Init sent, but no HID input followed");
        failed = 1;
    }

    if (failed == 0) {
        os_log(agent->log, "Controller input is flowing");
    } else if (++agent->attempt < PC2_MAX_ATTEMPTS) {
        os_log_info(agent->log, "Retrying (attempt %d of %d)", agent->attempt + 1, PC2_MAX_ATTEMPTS);
        CFRunLoopTimerSetNextFireDate(agent->timer,
                                      CFAbsoluteTimeGetCurrent() + PC2_RETRY_DELAY_S * agent->attempt);
    } else {
        os_log_error(agent->log, "Giving up until the next attach or wake");
    }
}

static void PC2_TimerCallback(CFRunLoopTimerRef timer, void *info)
{
    PC2_Agent *agent = (PC2_Agent *)info;

    /* Park the timer; PC2_EnableAll() re-arms it for retries */
    CFRunLoopTimerSetNextFireDate(timer, CFAbsoluteTimeGetCurrent() + PC2_TIMER_IDLE);
    PC2_EnableAll(agent);
}

/* Draining an iterator is required to arm its notification */
static int PC2_DrainIterator(io_iterator_t iterator)
{
    io_service_t service;
    int count = 0;

    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        IOObjectRelease(service);
        count++;
    }
    return count;
}

static void PC2_AttachCallback(void *refcon, io_iterator_t iterator)
{
    PC2_Agent *agent = (PC2_Agent *)refcon;

    if (PC2_DrainIterator(iterator) > 0) {
        PC2_Schedule(agent, PC2_ATTACH_DELAY_S, "Controller attached");
    }
}

static void PC2_DetachCallback(void *refcon, io_iterator_t iterator)
{
    PC2_Agent *agent = (PC2_Agent *)refcon;

    if (PC2_DrainIterator(iterator) > 0) {
        os_log_info(agent->log, "Controller detached");
    }
}

/* A streaming controller's stick noise counts as user activity and makes
   macOS abort sleep, so hide its input before allowing sleep. */
static void PC2_QuietAll(PC2_Agent *agent)
{
    const S2_Command *commands;
    size_t count = S2_GetUSBQuietSequence(&commands);
    int failed;

    /* Don't let a pending init undo this mid-transition */
    CFRunLoopTimerSetNextFireDate(agent->timer, CFAbsoluteTimeGetCurrent() + PC2_TIMER_IDLE);
    if (PC2_SendToAll(agent, commands, count, "quiet", &failed) > 0 && failed == 0) {
        os_log(agent->log, "Controller input hidden for sleep");
    }
}

static void PC2_PowerCallback(void *refcon, io_service_t service, natural_t message_type, void *message_argument)
{
    PC2_Agent *agent = (PC2_Agent *)refcon;

    (void)service;
    switch (message_type) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(agent->power_root, (long)message_argument);
        break;
    case kIOMessageSystemWillSleep:
        PC2_QuietAll(agent);
        IOAllowPowerChange(agent->power_root, (long)message_argument);
        break;
    case kIOMessageSystemHasPoweredOn:
        PC2_Schedule(agent, PC2_WAKE_DELAY_S, "System woke");
        break;
    default:
        break;
    }
}

static bool PC2_AddDeviceNotifications(PC2_Agent *agent)
{
    const uint16_t *product_ids;
    size_t num_products = PC2_USBGetProductIDs(&product_ids);

    if (num_products > PC2_MAX_PRODUCTS) {
        num_products = PC2_MAX_PRODUCTS;
    }
    agent->notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!agent->notify_port) {
        return false;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(agent->notify_port),
                       kCFRunLoopDefaultMode);

    for (size_t i = 0; i < num_products; i++) {
        CFMutableDictionaryRef attach = PC2_USBCreateMatching(product_ids[i]);
        CFMutableDictionaryRef detach = PC2_USBCreateMatching(product_ids[i]);
        kern_return_t kr;

        if (!attach || !detach) {
            return false;
        }
        /* Each call consumes its matching dictionary */
        kr = IOServiceAddMatchingNotification(agent->notify_port, kIOFirstMatchNotification, attach,
                                              PC2_AttachCallback, agent, &agent->attach_iterators[i]);
        if (kr != KERN_SUCCESS) {
            CFRelease(detach);
            return false;
        }
        agent->num_iterators = i + 1;
        kr = IOServiceAddMatchingNotification(agent->notify_port, kIOTerminatedNotification, detach,
                                              PC2_DetachCallback, agent, &agent->detach_iterators[i]);
        if (kr != KERN_SUCCESS) {
            return false;
        }
        PC2_DrainIterator(agent->detach_iterators[i]);
        /* Controllers already present count as attached */
        PC2_AttachCallback(agent, agent->attach_iterators[i]);
    }
    return true;
}

static bool PC2_AddPowerNotifications(PC2_Agent *agent)
{
    agent->power_root = IORegisterForSystemPower(agent, &agent->power_port, PC2_PowerCallback,
                                                 &agent->power_notifier);
    if (agent->power_root == MACH_PORT_NULL) {
        return false;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(agent->power_port),
                       kCFRunLoopDefaultMode);
    return true;
}

/* Make the mapping visible to apps launched from the Dock or Finder in this
   login session (launchd's environment, not ours) */
static void PC2_PublishMapping(PC2_Agent *agent, const char *mapping_path)
{
    char resolved[PATH_MAX];
    char *argv[] = { "launchctl", "setenv", "SDL_GAMECONTROLLERCONFIG_FILE", resolved, NULL };
    pid_t pid;
    int status = 0;

    /* Keep absolute paths as given: resolving Homebrew's stable opt/ symlink
       would pin a versioned Cellar path that disappears on upgrade */
    if (mapping_path[0] == '/' && strlen(mapping_path) < sizeof(resolved)) {
        strcpy(resolved, mapping_path);
    } else if (!realpath(mapping_path, resolved)) {
        os_log_error(agent->log, "Mapping file not found: %{public}s", mapping_path);
        return;
    }
    if (access(resolved, R_OK) != 0) {
        os_log_error(agent->log, "Mapping file not readable: %{public}s", resolved);
        return;
    }
    if (posix_spawn(&pid, "/bin/launchctl", NULL, NULL, argv, environ) != 0 ||
        waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        os_log_error(agent->log, "launchctl setenv failed for %{public}s", resolved);
        return;
    }
    os_log(agent->log, "Set SDL_GAMECONTROLLERCONFIG_FILE=%{public}s", resolved);
}

static void PC2_AgentCleanup(PC2_Agent *agent)
{
    for (size_t i = 0; i < agent->num_iterators; i++) {
        IOObjectRelease(agent->attach_iterators[i]);
        IOObjectRelease(agent->detach_iterators[i]);
    }
    if (agent->notify_port) {
        IONotificationPortDestroy(agent->notify_port);
    }
    if (agent->power_root != MACH_PORT_NULL) {
        IODeregisterForSystemPower(&agent->power_notifier);
        IOServiceClose(agent->power_root);
        IONotificationPortDestroy(agent->power_port);
    }
    if (agent->timer) {
        CFRunLoopTimerInvalidate(agent->timer);
        CFRelease(agent->timer);
    }
}

int PC2_AgentRun(const char *mapping_path)
{
    static PC2_Agent agent;
    CFRunLoopTimerContext timer_context = { 0, &agent, NULL, NULL, NULL };
    dispatch_source_t sigterm;
    int result = 0;

    agent.log = os_log_create(PC2_LOG_SUBSYSTEM, "agent");
    agent.power_root = MACH_PORT_NULL;
    os_log(agent.log, "Agent starting");

    if (mapping_path) {
        PC2_PublishMapping(&agent, mapping_path);
    }

    agent.timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + PC2_TIMER_IDLE,
                                       PC2_TIMER_IDLE, 0, 0, PC2_TimerCallback, &timer_context);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), agent.timer, kCFRunLoopDefaultMode);

    if (!PC2_AddPowerNotifications(&agent)) {
        os_log_error(agent.log, "Couldn't register for sleep/wake notifications");
        result = 1;
    } else if (!PC2_AddDeviceNotifications(&agent)) {
        os_log_error(agent.log, "Couldn't register for USB device notifications");
        result = 1;
    }

    if (result == 0) {
        /* launchd stops agents with SIGTERM */
        signal(SIGTERM, SIG_IGN);
        sigterm = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(sigterm, ^{
            CFRunLoopStop(CFRunLoopGetMain());
        });
        dispatch_resume(sigterm);

        CFRunLoopRun();

        dispatch_source_cancel(sigterm);
        dispatch_release(sigterm);
        os_log(agent.log, "Agent stopping");
    }

    PC2_AgentCleanup(&agent);
    return result;
}
