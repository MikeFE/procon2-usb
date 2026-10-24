# procon2-usb

Use your **Switch 2 Pro Controller** on a Mac over a USB-C cable, in
**Moonlight** and other games and apps built on SDL.

Out of the box, macOS sees the controller but gets no input from it. The
controller waits for a "start" message that only a Switch 2 normally sends.
procon2-usb runs quietly in the background and sends that message whenever
you plug the controller in or wake your Mac.

No drivers, no security changes, nothing to configure.

## Install

You need [Homebrew](https://brew.sh) and macOS 13 (Ventura) or later.

```sh
brew install mikefe/tap/procon2-usb
brew services start procon2-usb
```

Then **quit and reopen Moonlight** (only needed this first time).

## Use

1. Plug the controller into your Mac with a USB-C cable. Charge-only
   cables won't work.
2. Open Moonlight and play.

That's it. It keeps working after you unplug and replug the controller, and
after your Mac sleeps. The helper starts on its own when you log in.

## What works

| | |
|---|---|
| ✅ Buttons, sticks, d-pad, ZL/ZR, Home, +/− | Works |
| ⚠️ C, GL, GR (back buttons) | Not passed through when Moonlight streams to an Xbox 360-style controller, which has no such buttons |
| ❌ Rumble, motion controls, NFC, headset jack | Not supported |
| ❌ Bluetooth | USB only |

The face buttons follow the Xbox layout **by position**: the bottom button
(B on your controller) acts as Xbox "A", and the right button (A) acts as
Xbox "B". That matches what games on your streaming host expect.

**Which apps?** Moonlight, its forks, and other apps built on SDL, which
includes many emulators and ported games. Apps built on Apple's own Game
Controller framework won't see the controller. That covers many native Mac
games and Apple Arcade. For the same reason, it won't appear in System
Settings → Game Controllers.

## Troubleshooting

**Moonlight doesn't react to the controller.**
Quit and reopen Moonlight. Then check that the helper is running with
`brew services list`. It should say `started`.

**Check the controller itself.**
`procon2-usb monitor` shows buttons and sticks live as you press them.
Press Ctrl-C to stop.

**See what the helper is doing.**
```sh
log stream --predicate 'subsystem == "io.github.procon2-usb"' --info
```
Unplug and replug the controller; you should see "Controller input is
flowing".

## Uninstall

```sh
brew services stop procon2-usb
brew uninstall procon2-usb
```

## How it works

procon2-usb sends the controller the same start-up commands a Switch 2
would, using standard macOS USB access. The controller then shows up as a
regular USB gamepad, and the helper tells SDL apps which button is which.
Before your Mac sleeps, it briefly quiets the controller so it doesn't keep
your Mac awake.

Technical details, building from source, and the protocol notes are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Credits

The controller protocol comes from the reverse-engineering work in
[SDL](https://github.com/libsdl-org/SDL),
[linux-switch2](https://github.com/novakpetya/linux-switch2),
[procon2tool](https://github.com/HandheldLegend/procon2tool) and
[procon2-mac](https://github.com/caqlayan/procon2-mac).

## License

MIT. See [LICENSE](LICENSE).
