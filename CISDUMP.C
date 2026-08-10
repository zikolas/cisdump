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
 * Usage:  CISDUMP [/FULL] [/BIN file] [/S n] [/LEN n] [/?]
 *   (default)   decoded tuple dump of every socket that has a card
 *   /FULL /F    also decode CONFIG (COR base), CFTABLE_ENTRY (index, I/O
 *               base+len, IRQ, power), FUNCID names, FUNCE, DEVICE, JEDEC,
 *               all VERS_1 strings, and print a one-line SUMMARY per card
 *   /BIN file   also write the raw (de-interleaved) CIS bytes to <file>
 *   /S n        only scan socket n (0 or 1)
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

#define PCIC 0x3E0
static unsigned sockoff;

static void wr(unsigned char i, unsigned char v){ outp(PCIC, i + sockoff); outp(PCIC + 1, v); }
static unsigned char rd(unsigned char i){ outp(PCIC, i + sockoff); return (unsigned char)inp(PCIC + 1); }
static void dly(unsigned n){ while (n--) inp(0x80); }              /* ~1us each */
/* attribute memory: CIS byte i lives at window offset i*2 */
static unsigned char cisb(unsigned seg, unsigned i){ return *(unsigned char __far *)MK_FP(seg, i * 2); }

/* --- polite socket access -------------------------------------------------
 * Reading the CIS only needs attribute memory, which stays readable whenever
 * the card is powered - even on a card another enabler has already set up as an
 * I/O card. So a socket found already powered/enabled is not disturbed: we save
 * state, borrow a spare (disabled) memory window, read, then put the window
 * back exactly as it was. Only a card we had to power up ourselves is powered
 * back down (via unmapwin). */
static unsigned char sv02, sv03, sv06, svwin[6];
static int win_base, we_powered, was_io, rdy_timeout;
static int settle_polls, settle_unstable;
#define SETTLE_POLLS 250                /* settle gate cap: 250 x 20ms = ~5s */

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
        wr(0x02, 0x95); dly(20000);             /* power on, 5V   (we own it)    */
        wr(0x03, 0x40); dly(10000);             /* mem mode, reset released      */
        we_powered = 1;
    }
    /* else: leave power / reset / interface mode / IRQ exactly as found         */

    wn = free_memwin(sv06);
    win_base = 0x10 + wn * 8;                   /* mem window wn regs: 0x10+wn*8  */
    for (i = 0; i < 6; i++) svwin[i] = rd(win_base + i);   /* save window regs    */

    start = seg >> 8; stop = (seg >> 8) + 3;
    woff  = ((unsigned)(0 - (seg >> 8)) & 0x3FFF) | 0x4000;   /* 0x4000 = attribute space */
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
            if (b[0] != 0xFF) {                          /* clause 1 */
                for (k = 0; k < 4 && a[k] == b[k]; k++) ;
                if (k == 4) break;                       /* clause 2 */
            }
            for (k = 0; k < 4; k++) a[k] = b[k];         /* roll forward */
        }
        settle_polls = t;
    }
    rdy_timeout     = (cisb(seg, 0) == 0xFF);
    settle_unstable = (!rdy_timeout && settle_polls >= SETTLE_POLLS);
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
static int  s_irq, s_def_idx, s_have_io;

static void reset_summary(void)
{
    s_have_manf = s_have_func = s_have_io = 0;
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
    /* remember the default (or first) entry for the SUMMARY line */
    if (deflt || !s_have_io) {
        if (io_base >= 0) { s_io_base = io_base; s_io_len = io_len; s_have_io = 1; }
        if (irqmask >= 0)       s_irq = irqmask;
        else if (irqno >= 0)    s_irq = 1 << irqno;
        s_def_idx = index;
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
    printf("CISDUMP 1.1 - PCMCIA CIS reader/dumper (Intel 82365 PCIC @ 0x3E0)\n");
    printf("Usage: CISDUMP [/FULL] [/BIN file] [/S n] [/LEN n] [/?]\n");
    printf("  /FULL /F   decode CONFIG(COR), CFTABLE(I/O,IRQ), FUNCID, +SUMMARY\n");
    printf("  /BIN file  write raw de-interleaved CIS bytes to <file>\n");
    printf("  /S n       scan only socket n (0 or 1)\n");
    printf("  /LEN n     CIS bytes to write for /BIN (default 512)\n");
    printf("  /? /H      this help\n");
}

int main(int argc, char **argv)
{
    unsigned sock, seg = 0xD000;
    int full = 0, socksel = -1, binlen = 512, i;
    char *binfile = NULL;

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') a++;
        if      (!stricmp(a, "FULL") || !stricmp(a, "F")) full = 1;
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

    printf("CISDUMP 1.1 - PCMCIA CIS reader/dumper\n");
    for (sock = 0; sock < 2; sock++) {
        if (socksel >= 0 && (int)sock != socksel) continue;
        sockoff = sock * 0x40;
        printf("=== Socket %u (PCIC 0x3E0, bank 0x%02X) ===\n", sock, sockoff);
        if (!mapwin(seg)) { printf("  (no card present)\n"); continue; }
        if (we_powered)
            printf("  [socket was off: powered up to read, will power back down]\n");
        else
            printf("  [card already %s: reading CIS live, leaving it untouched]\n",
                   was_io ? "enabled as I/O" : "powered");
        if (settle_polls > 0)
            printf("  [socket is slow: CIS settled after %d x 20ms]\n", settle_polls);
        if (rdy_timeout)
            printf("  [warning: CIS still all-FF after a 5s wait - blank CIS, or\n"
                   "   this host needs longer than that to bring the socket up]\n");
        else if (settle_unstable)
            printf("  [warning: CIS never held still over a 5s wait - the bytes below\n"
                   "   are an un-settled read, NOT this card's CIS. Re-run it]\n");
        dumpcis(seg, full);
        if (binfile) write_bin(seg, binfile, binlen);
        unmapwin();                                      /* polite restore */
    }
    return 0;
}
