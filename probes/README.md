# probes

Bench instruments, not deliverables. Build on the box with `C:\WATCOM\BLD.BAT <name>`.

## CISWAIT.C — how long does this host take to serve a CIS?

Powers socket 0 fully down, brings it back up, and polls CIS byte 0 against the
BIOS tick counter (18.2 Hz, ~55 ms per tick) until it stops reading `0xFF`. It
also records when the PCIC READY bit asserts, and reports the gap between the
two.

This is the instrument behind the settle numbers in `~/Projects/pcmcia-cis-ff-bug.md`:

| Host | socket power → CIS readable |
|---|---|
| IBM PC110 | ready inside ~30 ms |
| ThinkPad 235 | ~110 ms |

and behind the reason **the READY bit is not a usable gate** — on the 235 it
asserts a full tick *before* attribute memory is readable, which is exactly what
the "RDY … BEFORE the CIS" line prints.

Run it when a new host joins the fleet, or any time a card looks blank on one
machine and fine on another. It leaves the socket powered off, as it found it.
