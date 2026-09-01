/* CISWAIT.C - measure how long a freshly-powered socket takes to serve its CIS.
 * Powers socket 0 down, then up, maps an attribute window and polls CIS byte 0
 * using the BIOS tick counter (18.2 Hz) until it stops reading 0xFF.
 * Build: C:\WATCOM\BLD.BAT CISWAIT
 */
#include <stdio.h>
#include <conio.h>
#include <dos.h>

#define PCIC 0x3E0
static unsigned sockoff = 0;

static void wr(unsigned char i, unsigned char v){ outp(PCIC, i + sockoff); outp(PCIC + 1, v); }
static unsigned char rd(unsigned char i){ outp(PCIC, i + sockoff); return (unsigned char)inp(PCIC + 1); }
static void dly(unsigned n){ while (n--) inp(0x80); }
static unsigned char cisb(unsigned seg, unsigned i){ return *(unsigned char __far *)MK_FP(seg, i * 2); }

static unsigned long ticks(void)
{
    return *(unsigned long __far *)MK_FP(0x40, 0x6C);   /* BIOS tick count */
}

int main(void)
{
    unsigned seg = 0xD000, start, stop, woff;
    unsigned long t0, t1, trdy = 0;
    int sawrdy = 0, i;

    printf("CISWAIT - socket 0 CIS settle time\n");

    /* cold: power the socket right down */
    wr(0x06, 0x00); wr(0x03, 0x00); wr(0x02, 0x00);
    dly(60000); dly(60000);

    if ((rd(0x01) & 0x0C) != 0x0C) { printf("no card in socket 0\n"); return 1; }

    t0 = ticks();
    wr(0x02, 0xB0); dly(20000);          /* power on 5V */
    wr(0x03, 0x40); dly(10000);          /* mem mode, reset released */

    start = seg >> 8; stop = (seg >> 8) + 3;
    woff  = ((unsigned)(0 - (seg >> 8)) & 0x3FFF) | 0x4000;
    wr(0x10, start & 0xFF); wr(0x11, (start >> 8) & 0x3F);
    wr(0x12, stop  & 0xFF); wr(0x13, (stop  >> 8) & 0x3F);
    wr(0x14, woff  & 0xFF); wr(0x15, (woff  >> 8) & 0xFF);
    wr(0x06, 0x01);

    /* poll: note when RDY asserts and when the CIS actually appears */
    for (i = 0; i < 20000; i++) {
        if (!sawrdy && (rd(0x01) & 0x20)) { trdy = ticks(); sawrdy = 1; }
        if (cisb(seg, 0) != 0xFF) break;
        dly(2000);
    }
    t1 = ticks();

    if (cisb(seg, 0) == 0xFF) {
        printf("CIS never appeared (still FF)\n");
    } else {
        printf("CIS byte0 = %02X after %lu ticks (~%lu ms)\n",
               cisb(seg, 0), t1 - t0, (t1 - t0) * 55);
        if (sawrdy)
            printf("RDY bit asserted after %lu ticks (~%lu ms) - %lu ticks BEFORE the CIS\n",
                   trdy - t0, (trdy - t0) * 55, t1 - trdy);
        else
            printf("RDY bit never asserted\n");
    }

    wr(0x06, 0x00); wr(0x03, 0x00); wr(0x02, 0x00);   /* leave it as we found it: off */
    return 0;
}
