# Development notes

## Build and test

    make              # build/procon2-usb: universal, ad-hoc signed, Info.plist embedded
    make ARCHS=       # build for this Mac's architecture only
    make test         # offline tests: protocol core and descriptor parser

Requires the Xcode Command Line Tools and macOS 13+. There are no other
dependencies.

To install from source without Homebrew:

    make install      # copies to ~/Library/Application Support/procon2-usb and loads a LaunchAgent
    make uninstall    # also runs launchctl unsetenv SDL_GAMECONTROLLERCONFIG_FILE

## Commands

| Command | What it does |
|---|---|
| `enable [-v] [--verify] [--format N]` | Send the init sequence. `-v` prints each reply, `--verify` waits for HID input afterwards, `--format` overrides the report format byte (default `0x09`). |
| `monitor [--raw]` | Live button and stick state. `--raw` prints changed report bytes. |
| `learn` | Guided button and stick mapping. Prints an SDL mapping string. |
| `descriptor [--hex]` | Dump and decode the HID report descriptor. |
| `agent [--sdl-mapping FILE]` | Background mode, run by launchd. |
| `quiet` | Switch to vendor report `0x05`, hiding input from macOS until the next `enable`. |
| `sequence` | Print the init sequence without sending it. |
| `send <hex bytes>` | Send one raw framed command and print the reply. |

Exit codes: 0 ok, 2 not found, 3 open/busy/permission, 4 init command failed,
5 no HID input after `--verify`, 64 usage.

Agent logs:

    log stream --predicate 'subsystem == "io.github.procon2-usb"' --info

## How it works

The controller (`057E:2069`) enumerates over USB but sends no input until it
gets an init sequence on its vendor-specific interface (interface 1, bulk
endpoints). A non-sandboxed process can open that interface with IOUSBLib
with no entitlement and no root. After init, the controller streams input
reports at 250 Hz on its HID interface, which macOS's own HID driver already
owns. Nothing virtual is created.

The init does not persist. The controller resets on unplug and on host
sleep, so the agent re-sends it on every attach and wake. Re-sending it to an
already active controller is safe.

### Report formats

The HID descriptor declares two input reports:

| Report | Contents |
|---|---|
| `0x05` | 63 bytes on vendor page `0x00FF`. This is SDL's native Switch 2 layout (`S2_ParseProReport`). |
| `0x09` | 2 vendor bytes, 21 Button-page buttons, 3 padding bits, X/Y/Rx/Rz at 12 bits each, then 52 vendor bytes (`S2_ParseHIDReport`). |

The "set report format" command (`03/0A`) picks which one the controller
sends. The init sequence is SDL 3.4's (`SDL_hidapi_switch2.c`) with the
format changed from `0x05` to `0x09`, because only report `0x09` has standard
usages that SDL's generic IOKit backend can read. The switch works live,
without re-init. Format `0x00` is rejected.

The sequence deliberately does not come from procon2-mac. That version sends
pairing commands (`0x15`, requesting the controller's MAC and LTK) and has
frames whose length byte doesn't match the payload.

Replies echo the command and subcommand bytes; byte 1 is a status (`0x01` ok).
On the Pro Controller, "enable rumble" and "enable grip buttons" reply `0x04`,
which is harmless.

### Moonlight and SDL

Moonlight 6.2.0 bundles SDL 3.4.18 built without libusb, so SDL's Switch 2
driver isn't compiled in and nothing in SDL claims the controller. SDL's
IOKit backend sees a generic HID gamepad and needs the mapping in
[sdl/gamecontrollerdb.txt](../sdl/gamecontrollerdb.txt), which `learn`
recorded. At startup the agent runs `launchctl setenv
SDL_GAMECONTROLLERCONFIG_FILE`, so apps launched from the Dock or Finder
pick it up. SDL supports that variable from SDL 2.0.22 on.

To test without the agent, launch the binary directly; `open -a` doesn't pass
environment variables:

    SDL_GAMECONTROLLERCONFIG_FILE="$PWD/sdl/gamecontrollerdb.txt" \
        /Applications/Moonlight.app/Contents/MacOS/Moonlight

If macOS's GameController framework ever claims the controller first, add
`SDL_JOYSTICK_MFI=0`.

Face buttons are mapped by position: Nintendo B (bottom) is SDL `a`. C, GL
and GR map to SDL `misc2`, `paddle1` and `paddle2`. They don't reach the host
when Sunshine emulates an Xbox 360 controller.

### Sleep

Stick noise changes about 230 of the 250 reports sent each second.
WindowServer's GamepadHIDServiceFilter counts that as user activity, so the
kernel aborted sleep ("Requesting full wake due to dark wake activity
tickle"). On `kIOMessageSystemWillSleep`, the agent switches controllers to
report `0x05` before acknowledging, so macOS sees no gamepad usages change.
Clearing the feature mask (`0C`) and sending "start output" `00` do not stop
the stream.

## Source layout

| File | Contents |
|---|---|
| `src/s2_proto.{h,c}` | Protocol core: init and quiet sequences, flash reads, calibration, report parsing. No I/O, no allocation. Shared with s2ble. |
| `src/pc2_usb.{h,c}` | IOUSBLib transport: IOPropertyMatch on IOUSBHostInterface, GetPipeProperties, WritePipeTO/ReadPipeTO, reply matching. |
| `src/pc2_hid.{h,c}` | IOHIDManager: monitor, input probe, descriptor decoder. |
| `src/pc2_learn.c` | Guided mapping, SDL GUID and mapping string. |
| `src/pc2_agent.{h,c}` | Background agent: attach and terminate notifications, sleep and wake, debounce and retry, os_log. |
| `packaging/homebrew/` | Homebrew formula template. |
| `scripts/` | Install and uninstall without Homebrew. |

## Releasing

1. Tag a release: `git tag v0.1.0 && git push origin v0.1.0`.
2. Get the tarball checksum:
   `curl -sL https://github.com/MikeFE/procon2-usb/archive/refs/tags/v0.1.0.tar.gz | shasum -a 256`.
3. Copy [packaging/homebrew/procon2-usb.rb](../packaging/homebrew/procon2-usb.rb)
   into the `MikeFE/homebrew-tap` repo as `Formula/procon2-usb.rb`, with the
   new `url` and `sha256`.
