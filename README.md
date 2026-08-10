# CISDUMP — a PCMCIA CIS reader/dumper for DOS

A small, card-agnostic DOS tool that reads a 16-bit PC Card's **CIS**
(Card Information Structure) through an Intel **82365-class PCIC** and decodes
it — the fast way to identify a card and read the facts in its CIS:
**MANFID**, **FUNCID**, the **COR** (Configuration Option Register) address,
and each configuration's **I/O windows** and **IRQ** options. One ~17 KB
`.EXE`, no Card Services or Socket Services required.

Tested on an IBM PC110 (PCIC at `3E0h`); it should work on any 82365-compatible
socket controller at that index.

## Polite by default

CISDUMP never disturbs a card that is already running. The CIS lives in
**attribute memory**, which stays readable whenever the card is powered — even
after another enabler has configured the card as an I/O device — so CISDUMP:

- **Reads an already-powered/enabled card live** and leaves it **exactly as it
  was**: its power, reset, interface mode and IRQ steering are never touched.
- **Borrows a free memory window** to reach attribute space: it scans the
  controller's window-enable register, picks the first *disabled* memory window,
  saves that window's registers, reads, then restores the window registers and
  the enable byte precisely — so it never clobbers the enabler's I/O windows.
- **Only powers down a card it powered up itself.** A card found already on is
  left on.
- **Waits for the CIS to actually appear** after powering a socket. How long a
  freshly-powered socket takes to serve attribute memory is a property of the
  **host**, not the card: an IBM PC110 is ready within ~30 ms, while a ThinkPad
  235 takes ~110 ms (measured, same cards). A fixed delay tuned on one machine
  reads too early on another and returns all-`FF` for *every* card — which is an
  easy way to mistake a perfectly healthy CIS for a wiped one. The PCIC's READY
  bit is no help either: on the 235 it asserts ~55 ms in, before the CIS is
  readable. CISDUMP instead polls byte 0 until a real tuple appears (a valid CIS
  never starts with the `0xFF` end marker), up to ~5 s, then says so if it
  stayed blank. On a fast host it exits on the first read and costs nothing.

It tells you which path it took:

```
  [card already enabled as I/O: reading CIS live, leaving it untouched]
  [socket was off: powered up to read, will power back down]
```

## Usage

```
CISDUMP [/FULL] [/BIN file] [/S n] [/LEN n] [/?]

  (default)   decoded tuple dump of every socket that has a card
  /FULL /F    also decode CONFIG (COR base), CFTABLE_ENTRY (index, I/O
              base+len, IRQ), FUNCID names, FUNCE, DEVICE, JEDEC, all
              VERS_1 strings, and print a one-line SUMMARY per card
  /BIN file   also write the raw (de-interleaved) CIS bytes to <file>
  /S n        only scan socket n (0 or 1)
  /LEN n      number of CIS bytes for /BIN (default 512)
  /? /H       help
```

### `/FULL` — decode, don't just dump

`/FULL` turns the raw tuple list into the numbers you actually need:

- **CISTPL_CONFIG** → the last configuration index and the **COR base address**
  in attribute space (plus the config-register presence mask).
- **CISTPL_CFTABLE_ENTRY** → each configuration's index, default flag, decoded
  **I/O base and length** (per range), and **IRQ** — the level/pulse/shared
  flags plus either the 16-bit interrupt mask expanded to an IRQ list, or a
  fixed IRQ number.
- **CISTPL_FUNCID** with a human name, **FUNCE**, **DEVICE**, **JEDEC**, and all
  **CISTPL_VERS_1** strings.
- A one-line **SUMMARY**: `MANFID / function / COR / default cfg# / I/O range`.

### `/BIN` — archive the raw CIS

`/BIN file` writes the de-interleaved CIS bytes to a file (default **512**
bytes — enough to capture cards whose MANFID sits past offset 256; use `/LEN`
to change).

## Build

16-bit real-mode, **Open Watcom 1.9**, small model. On the target DOS box:

```
BUILD.BAT            rem  wcc -ms CISDUMP.C  then  wlink ... CISDUMP.exe
```

The source is plain C89 (declarations at block top, no `//`), so it also builds
under Turbo C / other 16-bit compilers with minor tweaks. The linker's
`cannot open math87s.lib` warning is harmless — CISDUMP uses no floating point.
The prebuilt `CISDUMP.EXE` in this repo is built from this source, on hardware.

## How it works

Attribute memory is byte-interleaved: CIS byte *i* lives at attribute offset
*i*×2. CISDUMP maps a PCIC memory window onto attribute space (window offset bit
`0x4000` selects the attribute/`REG#` region), then walks the tuple chain —
each tuple is `code, link, link× data` — until the `0xFF` end marker, decoding
the ones that matter. The decoded output is organized around the fields you
reach for most: the COR/config-register block and the I/O options.

## Provenance

Clean-room: built from the public **Intel 82365SL** register set and the
**PCMCIA CIS tuple** specification (Card Metaformat / PC Card Standard, cross-
checked against Ralf Brown's Interrupt List and the Linux `i82365` register
definitions). No vendor driver code. It grew out of the throwaway `CISDUMP`
probe used across several DOS PCMCIA enabler projects, generalized into a
standalone tool.

## License

[MIT](LICENSE) © 2026 zikolas
