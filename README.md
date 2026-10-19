# nano7-a1717-dock

RAM patch for the iPod nano 7th generation that enables the Apple Lightning Dock A1717:
- the "This accessory is not supported" message is suppressed
- audio is routed to the dock's 3.5 mm line-out

The patch lives in RAM only and is gone after the next reboot. The firmware on flash is never modified.

**Experimental.** The mechanism was verified on one nano (firmware 1.1.2 / 39A10023) with a separate RAM tracing setup. This repository packages the same two changes as a standalone payload and loader; reports from other devices are welcome.

## Disclaimer

Use at your own risk. This is an unofficial hobby project with no guarantee that it works or is safe on your device. The patch only changes RAM, and a reboot of the nano normally restores the stock state. I have not had any problems with this patch, and nothing unrecoverable in my other nano experiments either. Still, running unofficial code on the iPod (including ipod_sun, which this depends on) can in the worst case leave it unusable. You are responsible for anything that happens to your device.

## Requirements

| | |
|---|---|
| Device | iPod nano 7G, firmware 1.1.2 (build 39A10023). The addresses are build-specific; `load.py` verifies the version before writing anything. |
| Code execution | [ipod_sun](https://github.com/CUB3D/ipod_sun), e.g. the untethered build from [ipod_sun_untethered](https://github.com/nfzerox/ipod_sun_untethered). Its vendor SCSI commands `c6 96 01/02/03` (write / read / call) are used to load the payload. |
| Build | `arm-none-eabi-gcc` |
| Loader | Linux: `sg_raw` (sg3_utils). macOS: `tools/macos/ipod_sg_raw.c`, built with `make ipod_sg_raw` (needs the Xcode command line tools). |

## Tested with

| | |
|---|---|
| iPod | iPod nano 7th generation (2012), MD481 (A1446), 16 GB, Slate |
| Firmware | 1.1.2 (39A10023) with ipod_sun untethered |
| Dock | Apple Lightning Dock A1717 |
| Host | macOS 26.6.2 |

## Build and load

```sh
git clone https://github.com/Raphmatt/nano7-a1717-dock.git
cd nano7-a1717-dock
make                                          # build/a1717_patch.bin

sudo python3 tools/load.py --dev /dev/sdX     # Linux

make ipod_sg_raw                              # macOS: build the SCSI tool once
python3 tools/load.py
```

The loader:
1. checks the firmware build, the hook site and the target RAM
2. uploads the payload and verifies it
3. runs the payload once and confirms the hook is installed

It has to be run again after each reboot.

## Layout

| Path | Contents |
|---|---|
| `src/` | the payload that runs on the nano (`a1717_patch.c`, linker script) |
| `tools/load.py` | uploads and runs the payload |
| `tools/macos/ipod_sg_raw.c` | `sg_raw`-compatible SCSI tool for macOS (IOKit `SCSITaskDeviceInterface`), rough first version |
| `tools/macos/nanoapps-macos-transport.patch` | adds macOS support to NanoApps' `start` script (see below) |

## macOS SCSI tool and NanoApps

`tools/macos/ipod_sg_raw.c` drives the iPod through IOKit's `SCSITaskDeviceInterface`, with the same command-line options as Linux `sg_raw`. It is a rough first version: functional, but not cleaned up yet. The plan is to submit it to [NanoApps](https://github.com/nfzerox/NanoApps) later.

It has only been used on one Mac (macOS 26.6.2) and may not work on every Mac or macOS version. Known shortcomings, to be fixed later:
- **Timeouts:** `-t` is in milliseconds, while `sg_raw -t` is in seconds. `load.py` doesn't pass `-t`, so the loader isn't affected.
- **Input checks:** missing for lengths, CDB bytes and option combinations. A short `-i` file is padded with zeros instead of being rejected.
- **Failed commands:** the output file is written even when the command fails.
- **Several iPods:** with more than one connected, the first one found is used.

Until it is upstream, it can be added to a NanoApps checkout by hand:

```sh
# next to your nano7-a1717-dock checkout:
git clone https://github.com/nfzerox/NanoApps.git
cd NanoApps
mkdir -p tools/macos
cp ../nano7-a1717-dock/tools/macos/ipod_sg_raw.c tools/macos/
git apply ../nano7-a1717-dock/tools/macos/nanoapps-macos-transport.patch
./start            # on macOS, builds tools/macos/ipod_sg_raw on first use
```

`nanoapps-macos-transport.patch` changes NanoApps' `start` script and `.gitignore`:
- **Mac as a local transport:** uses `ipod_sg_raw` instead of `sg_raw`, and `diskutil` for mount, eject and repair.
- **Automatic build:** compiles the tool on first use (needs `xcode-select --install`).
- **Setup and bootstrap checks:** extended to macOS.

The patch was made against NanoApps commit `80d439d` (2026-07-18). It contains parts of NanoApps' `start` script, which is MIT-licensed, Copyright (c) 2026 nfzerox.

## Background

Addresses are runtime addresses (osos at `0x08000000`, Thumb).

**Accessory check.** When a Lightning accessory is attached, the port controller (on SPI) returns a 26-byte identification record, which is stored at `0x08ae77a0`. The task at `0x08025358` accepts the accessory only if the vendor field is `0x05ac` and the type field is 1 or 2. For any other value it displays the unsupported-accessory screen. The A1717 reports type 5.

**Port power.** Acceptance alone does not enable the accessory. Port-controller lines 1 and 2 are switched on only by the port-power callback `0x080d0408(event 4, on)`. In the stock firmware that callback runs as part of USB host start, which is requested by an iAP1 accessory via the USB Host Control lingo. The A1717 sends no such request.

**Patch.** `src/a1717_patch.c` places an inline hook on `0x084df4c4`, the routine that copies the identification record. When the record matches the A1717 (vendor `05ac`, type `0005`), the hook:
1. changes the type to `0002` before the record is stored
2. queues a call on the UI task that waits for the port state (`0x08918558`) to become 3 and then calls `0x080d0408(4, 1)`

No USB enumeration takes place during playback.

Untested alternative to step 1: change the branch at `0x08025394` from `0d d0` to `0d e0`, which makes the check accept every Apple accessory type.

## Behaviour

| Dock rear port | Result |
|---|---|
| Not connected | audio |
| USB charger (connected before docking) | audio, nano charges |
| Computer | no identification record is returned, the check fails |

## Limitations

- Undock relies on the stock detach handling. The patch does not call `0x080d0408(4, 0)` itself.
- The payload occupies the unused area at the end of osos `.text` (`0x0867f8e4`). Other payloads that load there, such as NanoApps harnesses, overwrite it.
- Without a charger on the dock, the dock is powered by the nano's battery.

## Use of AI

This project was developed with AI assistance, using Anthropic's Claude models in Claude Code:

- **Claude Opus 5.5:** inspecting the firmware, the RAM recorder used to find the fix, the payload in `src/`, `tools/load.py` and this README.
- **Claude Fable 5.1:** `tools/macos/ipod_sg_raw.c` and `tools/macos/nanoapps-macos-transport.patch`.

Testing on the device and all decisions were done by me.

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Raphmatt.
