# CISDUMP — a PCMCIA CIS reader/dumper for DOS

A small, card-agnostic DOS tool that reads a 16-bit PC Card's **CIS**
(Card Information Structure) through an Intel **82365-class PCIC** and decodes
it — the fast way to identify a card and read the facts in its CIS:
**MANFID**, **FUNCID**, the **COR** (Configuration Option Register) address,
and each configuration's **I/O windows** and **IRQ** options. One ~17 KB
`.EXE`, no Card Services or Socket Services required.

Tested on an IBM PC110 and a ThinkPad 235; it should work with any
82365-compatible socket controller answering at `3E0/3E2/3E4/3E6`.

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
- **Never asserts Vpp.** Vpp is the card's programming supply, and on flash it is
  the interlock that makes a stray write physically unable to program or erase.
  A reader has no business raising it, so the socket is powered with Vpp off;
  `/VPP` exists for the rare card that will not read without it.
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
CISDUMP [/FULL] [/COMMON] [/RAW] [/FORCE] [/VPP] [/BIN file] [/S n] [/LEN n] [/?]

  (default)   decoded tuple dump of every socket that has a card
  /FULL /F    also decode CONFIG (COR base), CFTABLE_ENTRY (index, I/O
              base+len, IRQ), FUNCID names, FUNCE, DEVICE, JEDEC, all
              VERS_1 strings, and print a one-line SUMMARY per card
  /COMMON /C  read COMMON memory densely instead of attribute space —
              for memory-only cards, and cards that ignore REG#
  /RAW /R     do not de-interleave — read the window byte by byte
  /FORCE      parse as tuples even when the window is not a CIS
  /VPP        drive Vpp to Vcc while powering (off by default)
  /BIN file   also write the bytes as read (honours /COMMON and /RAW)
  /S n        only scan socket n (0–7, see Sockets below)
  /LEN n      number of CIS bytes for /BIN (default 512)
  /? /H       help
```

### Sockets and controllers

An 82365-class chip drives two sockets and answers at one of four index
ports, so socket *n* lives on the chip at `0x3E0 + (n & ~1)`, bank
`(n & 1) * 0x40` — the same numbering the DOS enablers use, so `/S` values
agree with theirs. CISDUMP scans sockets 0–7 across `3E0/3E2/3E4/3E6` and
checks each chip's identification register before touching it.

That check matters: an absent controller floats every read to `0xFF`, which
otherwise decodes as card-present *and* powered *and* I/O-configured, with an
all-`FF` window to match — a complete and entirely fictional card. Not
hypothetical: on a ThinkPad 235 the first bridge sits in CardBus mode, so
`0x3E0` is dead while the second answers PCIC at `0x3E4` with the card on it.

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
to change). It honours `/COMMON` and `/RAW`, so it can archive common memory
or the undoubled window just as easily.

### Cards with no CIS

Not every card has one. Memory-only cards may carry none at all, and a card
that ignores `REG#` serves its **common** memory into the attribute window —
so the "CIS" you read is really the front of a filesystem. Parsing that as
tuples invents plausible nonsense: one such card yields `TPL EB len 144` and a
fabricated `FUNCID`.

So CISDUMP classifies the window before believing it. A first byte that cannot
open a real tuple chain stops the parse and says why, and the boot-sector case
is named outright — every FAT volume opens with an x86 jump (`EB xx 90`), which
read de-interleaved off a `REG#`-ignoring card arrives as `EB 90`:

```
  no CIS in this window. First 16 bytes: EB 90 53 4F 35 30 ...
  -> x86 boot sector, not a tuple chain: this is a FAT volume.
     Read from ATTRIBUTE space, so the card is ignoring REG# and
     serving COMMON memory. No attribute memory means no CIS and
     no COR: it cannot be configured as an I/O card - memory mode
     only. Re-read it with /COMMON to see the bytes undoubled.
```

`/COMMON` then shows the sector properly — OEM, volume label and FAT type.
A card whose attribute memory is genuinely blank is reported as blank rather
than as a failure: that is a legitimate state, not an error.

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
