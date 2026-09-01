/* CISDUMP.C - dump a PCMCIA card's CIS via the Intel 82365 PCIC.
 * Scans sockets 0 and 1 (PCIC at 0x3E0), maps a memory window onto the card's
 * ATTRIBUTE memory, walks the CIS tuples and decodes them. Polite & read-only:
 * a card found already powered/enabled (e.g. configured by another enabler) is
 * read live and LEFT RUNNING - its power, reset, interface mode and IRQ are
 * never touched, the borrowed memory window is always restored, and only a card
 * we powered up ourselves is powered back down. Card-agnostic - use it to
 * identify any 16-bit PC Card and to plan enabler support (MANFID, FUNCID, the
 * COR address and the I/O / IRQ options live in the CIS).
 *
 * Usage:  CISDUMP [/FULL] [/COMMON] [/RAW] [/BIN file] [/S n] [/LEN n] [/?]
 *   (default)   decoded tuple dump of every socket that has a card
 *   /FULL /F    also decode CONFIG (COR base), CFTABLE_ENTRY (index, I/O
 *               base+len, IRQ, power), FUNCID names, FUNCE, DEVICE, JEDEC,
 *               all VERS_1 strings, and print a one-line SUMMARY per card
 *   /COMMON /C  read COMMON memory densely instead of attribute space, for
 *               memory-only cards and cards that ignore REG#
 *   /RAW /R     do not de-interleave - read the window byte by byte
 *   /FORCE      parse as tuples even when the window is not a CIS
 *   /VPP        drive Vpp to Vcc while powering. OFF by default: Vpp is the
 *               programming supply and a read-only tool should not assert it
 *   /BIN file   also write the bytes as read (honours /COMMON and /RAW)
 *   /S n        only scan socket n (0-7; chip at 3E0+(n&~1), bank (n&1))
 *   /LEN n      number of CIS bytes for /BIN (default 512)
 *   /?  /H      this help
 *
 * Build: C:\WATCOM\BLD.BAT CISDUMP
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

/* An 82365-class chip drives two sockets and answers at one of four index
   ports. Socket s lives on the chip at 0x3E0 + (s & ~1), bank (s & 1) * 0x40
   - the same mapping the enablers use, so /S numbers agree with theirs.
   1.3: NEVER assume a chip is there. An absent controller floats every read
   to 0xFF, which reads back as card-present + powered + I/O-configured and
   then as an all-FF window: a completely fabricated story about hardware
   that is not present. Seen for real on a ThinkPad 235 whose first bridge is
   in CardBus mode (0x3E0 dead) while the second answers PCIC at 0x3E4. */
#define PCIC_BASE 0x3E0
static unsigned pcic = PCIC_BASE;
static unsigned sockoff;

static void wr(unsigned char i, unsigned char v){ outp(pcic, i + sockoff); outp(pcic + 1, v); }
static unsigned char rd(unsigned char i){ outp(pcic, i + sockoff); return (unsigned char)inp(pcic + 1); }

/* Identification/revision register: an 82365-class part answers 10xxxxxxb. */
static int pcic_present(void)
{
    return (rd(0x00) & 0xC0) == 0x80;
}
static void dly(unsigned n){ while (n--) inp(0x80); }              /* ~1us each */
/* Attribute memory is byte-interleaved: CIS byte i lives at window offset i*2,
   so the default stride is 2. /RAW and /COMMON read densely (stride 1) - a card
   that ignores REG# serves common memory into the attribute window, and dense is
   the only way to see those bytes as they really are. */
static int rd_stride = 2;               /* 2 = de-interleaved, 1 = dense      */
static int rd_attr   = 1;               /* 1 = attribute space (REG#), 0 = common */
static unsigned char cisb(unsigned seg, unsigned i){ return *(unsigned char __far *)MK_FP(seg, i * rd_stride); }

/* --- polite socket access -------------------------------------------------
 * Reading the CIS only needs attribute memory, which stays readable whenever
 * the card is powered - even on a card another enabler has already set up as an
 * I/O card. So a socket found already powered/enabled is not disturbed: we save
 * state, borrow a spare (disabled) memory window, read, then put the window
 * back exactly as it was. Only a card we had to power up ourselves is powered
 * back down (via unmapwin). */
static unsigned char sv02, sv03, sv06, svwin[6];
static int win_base, we_powered, was_io, rdy_timeout;
static int settle_polls, settle_unstable, settle_capped;
/* Power control (reg 0x02) spelled out, because the old magic 0x95 hid what it
   was doing. Vpp1/Vpp2 are the card's PROGRAMMING supply; 0x95 drove BOTH to
   Vcc on every card this read-only tool powered. On flash that removes the
   hardware interlock which otherwise makes a stray write physically unable to
   program or erase - and era flash erases on a 2-cycle 0x20/0xD0 sequence.
   A reader has no business asserting Vpp, so it stays off unless asked. */
#define PWR_OUT   0x80                  /* card output enable                 */
#define PWR_AUTO  0x20                  /* auto power switch on card-detect   */
#define VCC_5V    0x10                  /* Vcc = 5.0V (bits 4:3; 0x18 = 3.3V) */
#define VPP1_VCC  0x01                  /* Vpp1 = Vcc   (0x02 would be 12V)   */
#define VPP2_VCC  0x04                  /* Vpp2 = Vcc   (0x08 would be 12V)   */
#define PWR_READ  (PWR_OUT|PWR_AUTO|VCC_5V)          /* Vpp OFF: the default  */
#define PWR_VPP   (PWR_READ|VPP1_VCC|VPP2_VCC)       /* only with /VPP        */
static int want_vpp;                    /* /VPP: some cards need Vpp to read  */

#define SETTLE_POLLS 250                /* settle gate cap: 250 x 20ms = ~5s */
#define SETTLE_FLOOR 15                 /* all-FF blank floor: 15 x 20ms = 300ms */

static int free_memwin(unsigned char wen)      /* first disabled mem window, 0..4 */
{
    int n;
    for (n = 0; n < 5; n++) if (!(wen & (1 << n))) return n;
    return 0;                                   /* none free: reuse #0 (saved/restored) */
}

static int mapwin(unsigned seg)
{
    unsigned start, stop, woff;
    int wn, i;
    if ((rd(0x01) & 0x0C) != 0x0C) return 0;    /* card-detect: not present */

    sv02 = rd(0x02); sv03 = rd(0x03); sv06 = rd(0x06);
    was_io = (sv03 & 0x20) != 0;                /* 0x20 = configured as an I/O card */
    we_powered = 0;
    /* power up only if the socket is off AND not already I/O-configured        */
    /* (0x40 in status reg 0x01 = power-on).                                     */
    if (!(rd(0x01) & 0x40) && !was_io) {
        wr(0x02, want_vpp ? PWR_VPP : PWR_READ); /* Vcc 5V, Vpp off (we own it)  */
        wr(0x03, 0x40); dly(10000);             /* mem mode, reset released      */
        we_powered = 1;
    }
    /* else: leave power / reset / interface mode / IRQ exactly as found         */

    wn = free_memwin(sv06);
    win_base = 0x10 + wn * 8;                   /* mem window wn regs: 0x10+wn*8  */
    for (i = 0; i < 6; i++) svwin[i] = rd(win_base + i);   /* save window regs    */

    start = seg >> 8; stop = (seg >> 8) + 3;
    woff  = ((unsigned)(0 - (seg >> 8)) & 0x3FFF);
    if (rd_attr) woff |= 0x4000;                /* 0x4000 = attribute space (REG#) */
    wr(win_base + 0, start & 0xFF); wr(win_base + 1, (start >> 8) & 0x3F);
    wr(win_base + 2, stop  & 0xFF); wr(win_base + 3, (stop  >> 8) & 0x3F);
    wr(win_base + 4, woff  & 0xFF); wr(win_base + 5, (woff  >> 8) & 0xFF);
    wr(0x06, sv06 | (1 << wn));                 /* enable just our borrowed window */
    dly(20000);

    /* Wait for the CIS to actually appear. How long a freshly-powered socket
       takes to serve attribute memory is a property of the HOST, not the card:
       an IBM PC110 is ready inside the old fixed ~30ms delay, while a ThinkPad
       235 takes ~110ms (measured, same cards) - so that delay read ~80ms too
       early there and returned all-FF for EVERY card, which is how a healthy
       CIS gets misdiagnosed as wiped. The PCIC RDY bit is not a usable gate
       either: on the 235 it asserts at ~55ms, a full tick before the CIS is
       readable. A valid CIS never starts with the 0xFF end marker, so poll
       byte 0 until it appears (~5s max, ample headroom over the ~110ms seen),
       then give up - a genuinely blank CIS stays 0xFF. On a fast host this
       exits on the first read and costs nothing.

       1.1: byte 0 alone is NOT enough. There are two un-settled modes, and
       only one of them is an FF wall: after a power CYCLE (residual charge,
       short off-time) a PC110 returns UNSTABLE NON-FF GARBAGE instead -
       "31 31 86 86" was caught sailing straight through a byte0-only gate.
       So the gate is two-clause: byte 0 != FF AND the first four dense bytes
       identical across two reads 20ms apart. An FF-ramp host waits on the
       first clause, a garbage-ramp host on the second; a genuinely blank
       card pays one full timeout and its verdict is then trustworthy. */
    {
        unsigned char a[4], b[4];
        int t, k;
        for (k = 0; k < 4; k++) a[k] = cisb(seg, k);
        for (t = 0; t < SETTLE_POLLS; t++) {
            dly(20000);                                  /* ~20ms */
            for (k = 0; k < 4; k++) b[k] = cisb(seg, k);
            for (k = 0; k < 4 && a[k] == b[k]; k++) ;
            if (k == 4) {                                /* clause 2: stable */
                if (b[0] != 0xFF) break;                 /* clause 1: has data */
                /* 1.2: all-FF AND stable. On an FF-ramp host the window also
                   reads a stable FF while it is still coming up, so a blank
                   verdict is only safe past a floor well clear of the ~110ms
                   worst case we have measured. Memory-only cards with no CIS
                   are a legitimate, common case - they must not pay 5s. */
                if (t >= SETTLE_FLOOR) break;
            }
            for (k = 0; k < 4; k++) a[k] = b[k];         /* roll forward */
        }
        settle_polls = t;
    }
    rdy_timeout     = (cisb(seg, 0) == 0xFF);
    settle_unstable = (!rdy_timeout && settle_polls >= SETTLE_POLLS);
    settle_capped   = (settle_polls >= SETTLE_POLLS);
    return 1;
}

static void unmapwin(void)
{
    int i;
    for (i = 0; i < 6; i++) wr(win_base + i, svwin[i]);   /* restore window regs  */
    wr(0x06, sv06);                                       /* restore enable byte  */
    if (we_powered) { wr(0x03, sv03); wr(0x02, sv02); }   /* power off only ours  */
    /* else: card stays powered/enabled exactly as we found it                   */
}

/* ---- decode helpers ------------------------------------------------------ */

static const char *funcname(int f)
{
    switch (f) {
    case 0:  return "Multifunction";
    case 1:  return "Memory";
    case 2:  return "Serial port / modem";
    case 3:  return "Parallel port / printer";
    case 4:  return "Fixed disk (ATA)";
    case 5:  return "Video adapter";
    case 6:  return "Network / LAN";
    case 7:  return "AIMS";
    case 8:  return "SCSI";
    case 9:  return "Security";
    case 10: return "Instrument";
    case 11: return "Multi-function serial (SIO)";
    default: return "Vendor-specific / unknown";
    }
}

/* per-card facts gathered for the /FULL summary */
static int  s_manf, s_prod, s_func;
static long s_cor;
static int  s_have_manf, s_have_func;
static long s_io_base, s_io_len;
static int  s_irq, s_def_idx, s_have_io, s_have_default;

static void reset_summary(void)
{
    s_have_manf = s_have_func = s_have_io = s_have_default = 0;
    s_cor = -1; s_io_base = s_io_len = -1;
    s_irq = -1; s_def_idx = -1; s_func = -1;
}

/* current tuple body, copied out of attribute space for safe indexed parsing */
static unsigned char body[256];
static int blen;

/* CISTPL_CONFIG (0x1A): pull last-index and the config-registers base (COR) */
static void parse_config(int full)
{
    int rasz, rmsz, i;
    long radr = 0;
    if (blen < 2) return;
    rasz = (body[0] & 0x03) + 1;
    rmsz = ((body[0] >> 2) & 0x03) + 1;
    s_def_idx = -1;                                  /* CONFIG carries LAST, not default */
    for (i = 0; i < rasz && (2 + i) < blen; i++) radr |= (long)body[2 + i] << (8 * i);
    s_cor = radr;
    if (full) {
        printf("    CONFIG: last cfg index %d, config regs @ attr 0x%lX (COR)\n",
               body[1] & 0x3F, radr);
        printf("      reg-present mask:");
        for (i = 0; i < rmsz && (2 + rasz + i) < blen; i++)
            printf(" %02X", body[2 + rasz + i]);
        printf("  (bit0=COR bit1=CCSR bit2=PRR bit3=SCR ...)\n");
    }
}

/* skip one variable field whose value bytes chain while bit7 is set */
static int skip_ext(int p)
{
    while (p < blen && (body[p] & 0x80)) p++;
    if (p < blen) p++;
    return p;
}

/* CISTPL_CFTABLE_ENTRY (0x1B): the meat - index, I/O base+len, IRQ, power */
static void parse_cftable(int full)
{
    int p = 0, index, deflt, iface = -1, fs, npwr, i;
    long io_base = -1, io_len = -1;
    int  io_lines = -1, irqno = -1, irqmask = -1, irqflags = 0;

    if (blen < 1) return;
    index = body[p] & 0x3F;
    deflt = (body[p] & 0x40) != 0;
    if (body[p] & 0x80) { p++; if (p < blen) iface = body[p]; }
    p++;
    if (p >= blen) goto emit;

    fs = body[p++];

    /* power description structures */
    npwr = fs & 0x03;
    while (npwr-- > 0 && p < blen) {
        int mask = body[p++], b;
        for (b = 0; b < 8; b++) if (mask & (1 << b)) p = skip_ext(p);
    }
    /* timing */
    if (fs & 0x04 && p < blen) {
        int td = body[p++];
        if ((td & 0x03) != 0x03)        p = skip_ext(p);   /* wait scale     */
        if (((td >> 2) & 0x07) != 0x07) p = skip_ext(p);   /* ready scale    */
        if (((td >> 5) & 0x07) != 0x07) p = skip_ext(p);   /* reserved scale */
    }
    /* I/O space */
    if (fs & 0x08 && p < blen) {
        int io = body[p++];
        io_lines = io & 0x1F;
        if (io & 0x80) {                                   /* range descriptor */
            int rng, nr, asz, lsz, r;
            if (p >= blen) goto irqpart;
            rng = body[p++];
            nr  = (rng & 0x0F) + 1;
            asz = (rng >> 4) & 0x03;  asz = (asz == 3) ? 4 : asz;
            lsz = (rng >> 6) & 0x03;  lsz = (lsz == 3) ? 4 : lsz;
            for (r = 0; r < nr && p < blen; r++) {
                long a = 0, l = 0;
                for (i = 0; i < asz && p < blen; i++) a |= (long)body[p++] << (8 * i);
                for (i = 0; i < lsz && p < blen; i++) l |= (long)body[p++] << (8 * i);
                if (r == 0) { io_base = a; io_len = l + 1; }
                if (full)
                    printf("      I/O range %d: base 0x%lX len %ld\n", r, a, l + 1);
            }
        }
    }
irqpart:
    /* IRQ description structure - one flags byte, then an optional mask:
     *   b7 Share  b6 Pulse  b5 Level  b4 Mask  b3-0 IRQ number
     * When b4 (Mask) is set a 16-bit little-endian IRQ mask FOLLOWS and the
     * low nibble is not an IRQ number - reading it as one prints a bogus
     * fixed IRQ (the Canon CE303 says 30 00 0C = level, IRQ 10 or 11).      */
    if (fs & 0x10 && p < blen) {
        int ir = body[p++];
        irqflags = ir & 0xE0;
        if (ir & 0x10) {                                   /* 2-byte IRQ mask */
            if (p + 1 < blen) irqmask = body[p] | (body[p + 1] << 8);
            p += 2;
        } else {
            irqno = ir & 0x0F;
        }
    }
    /* memory / misc fields after IRQ are not decoded (not needed to ID cards) */

emit:
    if (full) {
        printf("    CFTABLE entry #%d%s", index, deflt ? " (default)" : "");
        if (iface >= 0)
            printf(", iface %s", (iface & 0x0F) == 1 ? "I/O" :
                                 (iface & 0x0F) == 0 ? "memory" : "custom");
        if (io_lines >= 0) printf(", %d addr lines", io_lines);
        printf("\n");
        if (io_base >= 0)
            printf("      -> I/O 0x%lX..0x%lX (len %ld)\n",
                   io_base, io_base + io_len - 1, io_len);
        if (irqmask >= 0 || irqno >= 0) {
            printf("      -> IRQ ");
            if (irqflags & 0x20) printf("level ");
            if (irqflags & 0x40) printf("pulse ");
            if (irqflags & 0x80) printf("shared ");
            if (irqmask >= 0) {
                printf("mask 0x%04X {", irqmask);
                for (i = 0; i < 16; i++) if (irqmask & (1 << i)) printf(" %d", i);
                printf(" }\n");
            } else {
                printf("%d (fixed)\n", irqno);
            }
        }
    }
    /* SUMMARY: latch the FIRST default entry and let no later one replace it.
       A card may flag several - in the CIS the bit means "supplies defaults for
       the entries that follow", not "the preferred one" - and it is the first
       that an enabler actually configures. The Roland SCP-55 flags both #1 and
       #13; taking the last reported I/O 0xE0D0 while SCP55GO configures 0x330,
       which is precisely the question this line exists to answer. With no
       default anywhere, fall back to the first entry that carries I/O - and a
       real default still supersedes that fallback. */
    if (!s_have_default && (deflt || !s_have_io)) {
        if (io_base >= 0) { s_io_base = io_base; s_io_len = io_len; s_have_io = 1; }
        if (irqmask >= 0)       s_irq = irqmask;
        else if (irqno >= 0)    s_irq = 1 << irqno;
        s_def_idx = index;
        if (deflt) s_have_default = 1;
    }
}

/* CISTPL_DEVICE (0x01) / DEVICE_A (0x17): device-type + size, brief */
static void parse_device(void)
{
    static const char *dt[8] = { "NULL","ROM","OTPROM","EPROM",
                                 "EEPROM","FLASH","SRAM","DRAM" };
    int t;
    if (blen < 1 || body[0] == 0xFF) return;
    t = (body[0] >> 4) & 0x0F;
    printf("      device type %s, speed code 0x%02X\n",
           t < 8 ? dt[t] : "reserved", body[0] & 0x07);
}

static void decode(unsigned seg, int code, int off, int full)
{
    int i;
    (void)off;
    switch (code) {
    case 0x15:                                          /* CISTPL_VERS_1 */
        printf("    VERS_1 v%u.%u: ", body[0], body[1]);
        /* strings start at body index 2 (major/minor are 0,1); print each */
        for (i = 2; i < blen; i++) {
            unsigned char c = body[i];
            putchar(c ? c : '|');                        /* 0x00 separates strings */
        }
        printf("\n");
        break;
    case 0x20:                                          /* CISTPL_MANFID */
        if (blen >= 4) {
            s_manf = body[1] << 8 | body[0];
            s_prod = body[3] << 8 | body[2];
            s_have_manf = 1;
            printf("    MANFID: %04X / %04X\n", s_manf, s_prod);
        }
        break;
    case 0x21:                                          /* CISTPL_FUNCID */
        if (blen >= 1) {
            s_func = body[0]; s_have_func = 1;
            printf("    FUNCID: %02X (%s)\n", body[0], funcname(body[0]));
        }
        break;
    case 0x22:                                          /* CISTPL_FUNCE */
        if (full) { printf("    FUNCE:"); for (i=0;i<blen;i++) printf(" %02X",body[i]); printf("\n"); }
        break;
    case 0x1A:                                          /* CISTPL_CONFIG */
        parse_config(full);
        break;
    case 0x1B:                                          /* CISTPL_CFTABLE_ENTRY */
        parse_cftable(full);
        break;
    case 0x01: case 0x17:                               /* CISTPL_DEVICE(_A) */
        if (full) parse_device();
        break;
    case 0x18:                                          /* CISTPL_JEDEC_C */
        if (full && blen >= 2) printf("    JEDEC: mfr 0x%02X info 0x%02X\n", body[0], body[1]);
        break;
    case 0x13:                                          /* CISTPL_LINKTARGET */
        if (full) printf("    LINKTARGET\n");
        break;
    case 0x14:                                          /* CISTPL_NO_LINK */
        if (full) printf("    NO_LINK\n");
        break;
    case 0x10:                                          /* CISTPL_CHECKSUM */
        if (full) printf("    CHECKSUM\n");
        break;
    default:
        break;
    }
}

/* ---- what is actually in this window? ------------------------------------
 * Not every card has a CIS. Memory-only cards (and hosts with memory-only
 * sockets) may carry none at all, and a card that ignores REG# serves its
 * COMMON memory into the attribute window instead - so the "CIS" you read is
 * really the front of a filesystem. Parsing that as tuples invents plausible
 * nonsense: a New Media PalmModem yields 'TPL EB len 144' and a fabricated
 * FUNCID 40. Classify the bytes before believing them. */
#define WIN_CIS     0                   /* a plausible tuple chain            */
#define WIN_BLANK   1                   /* all-FF: no CIS present             */
#define WIN_BOOTSEC 2                   /* x86 boot sector = a FAT volume     */
#define WIN_NOTCIS  3                   /* something, but not a tuple chain   */
static int win_kind;

/* Codes a real CIS can legitimately open with. Everything else - notably the
   vendor-unique 0x80-0xFE range - is not a first tuple. */
static int plausible_first_tuple(int c)
{
    return c == 0x00 || c == 0x01 ||
           (c >= 0x10 && c <= 0x23) ||          /* CHECKSUM..SWIL             */
           (c >= 0x40 && c <= 0x46);            /* VERS_2..ORG                */
}

static int classify(unsigned seg)
{
    int c0 = cisb(seg, 0), c1 = cisb(seg, 1), c2 = cisb(seg, 2);
    if (c0 == 0xFF) return WIN_BLANK;
    /* Every FAT boot sector opens with an x86 jump: EB xx 90, or E9 xx xx.
       Read de-interleaved off a card that ignores REG#, EB 3C 90 arrives as
       EB 90 - that pair is the tell for this whole class of card. */
    if (c0 == 0xEB && ((rd_stride == 2 && c1 == 0x90) ||
                       (rd_stride == 1 && c2 == 0x90))) return WIN_BOOTSEC;
    if (c0 == 0xE9) return WIN_BOOTSEC;
    if (!plausible_first_tuple(c0)) return WIN_NOTCIS;
    return WIN_CIS;
}

/* Print the OEM/label text a boot sector carries, skipping unprintables. */
static void show_text(unsigned seg, int from, int len)
{
    int i;
    for (i = from; i < from + len; i++) {
        int c = cisb(seg, i);
        putchar((c >= 0x20 && c < 0x7F) ? c : '.');
    }
}

static void report_notcis(unsigned seg)
{
    int i;
    printf("  no CIS in this window. First 16 bytes:");
    for (i = 0; i < 16; i++) printf(" %02X", cisb(seg, i));
    printf("\n");
    if (win_kind == WIN_BOOTSEC) {
        printf("  -> x86 boot sector, not a tuple chain: this is a FAT volume.\n");
        if (rd_stride == 1) {                   /* dense: real FAT offsets   */
            printf("     OEM '"); show_text(seg, 3, 8);
            printf("'  label '"); show_text(seg, 0x2B, 11);
            printf("'  type '"); show_text(seg, 0x36, 8); printf("'\n");
        } else {                                /* half-sampled: fields halve */
            printf("     half-sampled text: '"); show_text(seg, 1, 36);
            printf("'\n");
        }
        if (rd_attr)
            printf("     Read from ATTRIBUTE space, so the card is ignoring REG# and\n"
                   "     serving COMMON memory. No attribute memory means no CIS and\n"
                   "     no COR: it cannot be configured as an I/O card - memory mode\n"
                   "     only. Re-read it with /COMMON to see the bytes undoubled.\n");
    } else {
        printf("  -> first byte %02X is not a valid opening tuple. Either this card\n"
               "     has no CIS, or the window is not showing attribute memory.\n"
               "     Try /COMMON (is it a memory card?) and /RAW. /FORCE parses anyway.\n",
               cisb(seg, 0));
    }
}

static void dumpcis(unsigned seg, int full)
{
    int off = 0, i, code, link, guard = 0;
    reset_summary();
    for (;;) {
        code = cisb(seg, off);
        if (code == 0xFF) { printf("  TPL FF (end)\n"); break; }
        if (code == 0x00) { off++; if (++guard > 512) break; continue; }
        link = cisb(seg, off + 1);
        /* copy the body out for safe indexed decoding */
        blen = link;
        if (blen > (int)sizeof(body)) blen = sizeof(body);
        for (i = 0; i < blen; i++) body[i] = cisb(seg, off + 2 + i);

        printf("  TPL %02X len %2d:", code, link);
        for (i = 0; i < blen && i < 24; i++) printf(" %02X", body[i]);
        if (link > 24) printf(" ...");
        printf("\n");

        decode(seg, code, off, full);

        if (link == 0xFF) break;
        off += link + 2;
        if (off > 0x400) break;
    }

    if (full) {
        printf("  SUMMARY:");
        if (s_have_manf) printf(" MANFID %04X/%04X", s_manf, s_prod);
        if (s_have_func) printf(" | %s", funcname(s_func));
        if (s_cor >= 0)  printf(" | COR@attr 0x%lX", s_cor);
        if (s_def_idx >= 0) printf(" | cfg#%d", s_def_idx);
        if (s_have_io)   printf(" | I/O 0x%lX..0x%lX", s_io_base, s_io_base + s_io_len - 1);
        if (s_irq >= 0)  printf(" | IRQmask 0x%04X", s_irq);
        printf("\n");
    }
}

static int write_bin(unsigned seg, const char *fn, int len)
{
    FILE *f = fopen(fn, "wb");
    int i;
    if (!f) { printf("  ! cannot open %s for writing\n", fn); return 0; }
    for (i = 0; i < len; i++) fputc(cisb(seg, i), f);
    fclose(f);
    printf("  wrote %d CIS bytes -> %s\n", len, fn);
    return 1;
}

static void usage(void)
{
    printf("CISDUMP 1.4 - PCMCIA CIS reader/dumper (Intel 82365 PCIC)\n");
    printf("Usage: CISDUMP [/FULL] [/COMMON] [/RAW] [/BIN file] [/S n] [/LEN n] [/?]\n");
    printf("  /FULL /F   decode CONFIG(COR), CFTABLE(I/O,IRQ), FUNCID, +SUMMARY\n");
    printf("  /COMMON /C read COMMON memory densely, not attribute space -\n");
    printf("             for memory-only cards, and cards that ignore REG#\n");
    printf("  /RAW /R    do not de-interleave: read the window byte by byte\n");
    printf("  /FORCE     parse as tuples even if it does not look like a CIS\n");
    printf("  /VPP       drive Vpp to Vcc while powering (OFF by default - a\n");
    printf("             reader should not assert programming voltage). Only\n");
    printf("             for a card that will not read without it\n");
    printf("  /BIN file  write the bytes as read (honours /COMMON and /RAW)\n");
    printf("  /S n       scan only socket n (0-7; chip 3E0+(n&~1), bank (n&1))\n");
    printf("  /LEN n     CIS bytes to write for /BIN (default 512)\n");
    printf("  /? /H      this help\n");
}

int main(int argc, char **argv)
{
    unsigned sock, seg = 0xD000;
    int full = 0, socksel = -1, binlen = 512, i, force = 0, nfound = 0;
    char *binfile = NULL;

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') a++;
        if      (!stricmp(a, "FULL") || !stricmp(a, "F")) full = 1;
        else if (!stricmp(a, "COMMON") || !stricmp(a, "C")) { rd_attr = 0; rd_stride = 1; }
        else if (!stricmp(a, "RAW") || !stricmp(a, "R")) rd_stride = 1;
        else if (!stricmp(a, "FORCE")) force = 1;
        else if (!stricmp(a, "VPP")) want_vpp = 1;
        else if (!stricmp(a, "BIN")  || !stricmp(a, "B")) {
            if (i+1 < argc && argv[i+1][0] != '/' && argv[i+1][0] != '-') binfile = argv[++i];
            else binfile = "CIS.BIN";
        }
        else if (!stricmp(a, "S")) { if (i+1<argc) socksel = atoi(argv[++i]); }
        else if (!stricmp(a, "LEN")||!stricmp(a,"L")) { if (i+1<argc) binlen = atoi(argv[++i]); }
        else if (!stricmp(a, "?") || !stricmp(a, "H") || !stricmp(a, "HELP")) { usage(); return 0; }
        else { printf("unknown option: %s\n", argv[i]); usage(); return 1; }
    }
    if (binlen < 1)   binlen = 512;
    if (binlen > 1024) binlen = 1024;

    printf("CISDUMP 1.4 - PCMCIA CIS reader/dumper\n");
    for (sock = 0; sock < 8; sock++) {
        if (socksel >= 0 && (int)sock != socksel) continue;
        pcic    = PCIC_BASE + (sock & ~1);
        sockoff = (sock & 1) * 0x40;
        if (!pcic_present()) {          /* no chip here - say nothing unless asked */
            if (socksel >= 0)
                printf("=== Socket %u ===\n  no 82365-class controller at 0x%03X\n",
                       sock, pcic);
            continue;
        }
        nfound++;
        printf("=== Socket %u (PCIC 0x%03X, bank 0x%02X, ID %02X) ===\n",
               sock, pcic, sockoff, rd(0x00));
        if (!mapwin(seg)) { printf("  (no card present)\n"); continue; }
        if (we_powered)
            printf("  [socket was off: powered up to read, will power back down]\n");
        else
            printf("  [card already %s: reading CIS live, leaving it untouched]\n",
                   was_io ? "enabled as I/O" : "powered");
        if (rd(0x01) & 0x10) printf("  [card is write-protected (-WP asserted)]\n");
        if (settle_polls > 0)
            printf("  [socket is slow: CIS settled after %d x 20ms]\n", settle_polls);
        if (rdy_timeout && settle_capped)
            printf("  [warning: still all-FF after a 5s wait - blank, or this host\n"
                   "   needs longer than that to bring the socket up]\n");
        else if (settle_unstable)
            printf("  [warning: CIS never held still over a 5s wait - the bytes below\n"
                   "   are an un-settled read, NOT this card's CIS. Re-run it]\n");
        win_kind = classify(seg);
        if (win_kind == WIN_CIS || force) {
            if (win_kind != WIN_CIS)
                printf("  [/FORCE: parsing as tuples anyway - see the warning above]\n");
            dumpcis(seg, full);
        } else if (win_kind == WIN_BLANK) {
            printf("  no CIS: %s reads all-FF%s.\n",
                   rd_attr ? "attribute memory" : "common memory",
                   settle_capped ? " and never settled" : " (settled)");
            if (rd_attr)
                printf("  -> a memory-only card with no CIS reads exactly like this.\n"
                       "     Try /COMMON to see whether it carries data.\n");
        } else {
            report_notcis(seg);
        }
        if (binfile) write_bin(seg, binfile, binlen);
        unmapwin();                                      /* polite restore */
    }
    if (!nfound)
        printf("No 82365-class PCIC found (scanned 3E0/3E2/3E4/3E6).\n"
               "A bridge in CardBus mode does not answer here - its sibling may\n"
               "still be in PCIC mode on a higher socket number.\n");
    return 0;
}
