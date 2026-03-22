# FuSa GAMEPAD (Dual Axis Fork)

Fork of [andy-man/psp-fusa-gamepad](https://github.com/andy-man/psp-fusa-gamepad) — turns your PSP into a USB gamepad.

## What's different in this fork

The original FuSa forces you to choose between D-pad OR analog stick as axes — never both at the same time. This fork fixes that.

**D-pad and analog stick now work simultaneously on separate axes:**

| Input | Axes | Values |
|---|---|---|
| D-pad | X / Y | 0 (left/up), 127 (center), 255 (right/down) |
| Analog stick | Z / Rz | 0–255 (continuous) |
| D-pad | POV hat | Kept for compatibility |

No more DigitalSwitcher, no more WLAN toggle — both inputs are always active.

## Install

1. Copy the `release/FusaGamePad/` folder to `PSP:/PSP/GAME/FusaGamePad/`
2. Launch FuSa GamePad from the XMB
3. Connect USB — your PSP is a gamepad

## Pre-built binaries

Ready-to-use files are in the `release/` folder. No need to compile.

## Mapping in PPSSPP (or other emulators)

- **D-pad directions** → map to axes X/Y
- **Analog stick** → map to axes Z/Rz
- **Buttons** → same as original (Cross, Circle, Square, Triangle, L, R, etc.)

## Build from source

Requires [PSPSDK](https://github.com/pspdev/pspdev).

```bash
export PSPDEV=~/pspdev
export PATH=$PATH:$PSPDEV/bin

cd src/prx && make
cd ../app && make
```

Output: `src/prx/usbgamepad.prx` + `src/app/EBOOT.PBP`

## Compatibility

Any PSP (Phat, Slim, 3000) running CFW. Standard HID device — no drivers needed on any OS.

## 12 Buttons

| Button | PSP |
|---|---|
| 1 | Cross |
| 2 | Circle |
| 3 | Square |
| 4 | Triangle |
| 5 | L-Trigger |
| 6 | R-Trigger |
| 7 | Start |
| 8 | Select |
| 9 | Home |
| 10 | Vol Down |
| 11 | Vol Up |
| 12 | Display |

## Credits

- **Original**: Andy_maN, hnaves, Forb — [FuSa TeaM](http://foosa.do.am) (2010)
- **This fork**: [Noeme](https://github.com/romainguerif) (2026)
