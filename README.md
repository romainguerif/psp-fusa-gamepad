# FuSa GAMEPAD v3.2 — Dual Input Fork

Fork of [andy-man/psp-fusa-gamepad](https://github.com/andy-man/psp-fusa-gamepad) — turns your PSP into a USB gamepad with full dual-input support.

## What's different

The original FuSa forces you to choose between D-pad OR analog stick — only one works at a time, toggled by the WLAN switch. This fork fixes that completely.

**D-pad and analog stick work simultaneously:**

| Input | HID mapping | Type |
|---|---|---|
| Analog stick | X / Y axes | Continuous (0–255) |
| D-pad | Z / Rz axes + Buttons 13–16 | Digital (0 / 127 / 255) |
| Face buttons | Buttons 1–4 | Cross, Circle, Square, Triangle |
| Shoulders | Buttons 5–6 | L Trigger, R Trigger |
| System | Buttons 9–10 | Select, Start |
| D-pad | Buttons 13–16 | Up, Down, Left, Right |

### What we rebuilt

The original FuSa HID descriptor had several issues on macOS:
- POV hat not recognized by PPSSPP on macOS
- Z/Rz axes treated as triggers (single-axis) instead of directional axes
- Button positions didn't match the standard gamepad layout, causing Select/RTrigger to merge
- DigitalSwitcher/AxisSwitcher code missing from the open-source repo (was in the closed v0.3 binary)

We rewrote the HID report descriptor from scratch, based on the DualShock 3/4 structure that is proven to work on macOS, Windows, and Linux:
- Standard gamepad button layout (same order as Xbox/PlayStation controllers)
- All 4 axes in a single Physical collection (X, Y, Z, Rz)
- D-pad sent as both axes AND buttons for maximum compatibility
- No POV hat (unreliable across platforms)
- No DigitalSwitcher toggle — both inputs are always active

## Install

1. Copy the `release/FusaGamePad/` folder to `PSP:/PSP/GAME/FusaGamePad/`
2. Launch FuSa GamePad from the XMB
3. Connect USB — your PSP is a gamepad

## Pre-built binaries

Ready-to-use files are in the `release/` folder. No need to compile.

## Button mapping in PPSSPP

| PSP button | Gamepad button | Standard name |
|---|---|---|
| Cross | B1 | A |
| Circle | B2 | B |
| Square | B3 | X |
| Triangle | B4 | Y |
| L Trigger | B5 | Left Shoulder |
| R Trigger | B6 | Right Shoulder |
| Select | B9 | Back |
| Start | B10 | Start |
| D-pad Up | B13 | D-pad Up |
| D-pad Down | B14 | D-pad Down |
| D-pad Left | B15 | D-pad Left |
| D-pad Right | B16 | D-pad Right |

For the analog stick, map Left Stick X/Y in PPSSPP's control settings.

## Build from source

Requires [PSPSDK](https://github.com/pspdev/pspdev).

```bash
export PSPDEV=~/pspdev
export PATH=$PATH:$PSPDEV/bin

cd src/prx && make
cd ../app && make
```

## Compatibility

- Any PSP (Phat, Slim, 3000) running CFW
- Standard HID device — no drivers needed
- Tested on macOS, should work on Windows and Linux

## Credits

- **Original**: Andy_maN, hnaves, Forb — [FuSa TeaM](http://foosa.do.am) (2010)
- **This fork**: [romainguerif](https://github.com/romainguerif) (2026)
