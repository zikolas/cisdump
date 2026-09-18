/* Host regression harness: execute the real source with simulated DOS I/O. */
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char regs[8][64], selectors[4];
static unsigned char memory[8][16384], common_memory[8][16384];
static unsigned common_reads;
static unsigned reads, writes, live_mapping_writes;
static char output[65536];
static size_t output_len;

static int capture_printf(const char *fmt, ...)
{
    int n;
    va_list args;
    va_start(args, fmt);
    n = vsnprintf(output + output_len, sizeof(output) - output_len, fmt, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(output) - output_len);
    output_len += (size_t)n;
    return n;
}

static int capture_putchar(int c)
{
    assert(output_len + 1 < sizeof(output));
    output[output_len++] = (char)c;
    output[output_len] = 0;
    return c;
}

static int test_stricmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

#define printf capture_printf
#define putchar capture_putchar
#define stricmp test_stricmp
#define main cisdump_main
#include "../CISDUMP.C"
#undef main
#undef stricmp
#undef putchar
#undef printf

int inp(unsigned port)
{
    unsigned chip, socket;
    if (port == 0x80) return 0;
    assert(port >= 0x3E1 && port <= 0x3E7 && (port & 1));
    chip = (port - 0x3E1) / 2;
    socket = chip * 2 + (selectors[chip] >> 6);
    reads++;
    return regs[socket][selectors[chip] & 0x3F];
}

int outp(unsigned port, int value)
{
    unsigned chip, socket, reg, window;
    assert(port >= 0x3E0 && port <= 0x3E7);
    chip = (port - 0x3E0) / 2;
    if (!(port & 1)) {
        selectors[chip] = (unsigned char)value;
        return value;
    }
    socket = chip * 2 + (selectors[chip] >> 6);
    reg = selectors[chip] & 0x3F;
    if (reg >= 0x10 && reg <= 0x35 && ((reg - 0x10) % 8) < 6) {
        window = (reg - 0x10) / 8;
        if (regs[socket][6] & (1U << window)) live_mapping_writes++;
    }
    regs[socket][reg] = (unsigned char)value;
    writes++;
    return value;
}

static unsigned char bus_float[16384], host_ram[16384];
static int shadow_host;
static unsigned window_seg = 0xD000;

unsigned char *test_mk_fp(unsigned seg, unsigned off)
{
    unsigned socket = (pcic - PCIC_BASE) + sockoff / 0x40;
    assert(seg == window_seg && off < sizeof(memory[0]));
    if (shadow_host) return host_ram + off;              /* a UMB shadows the window */
    if (!(regs[socket][6] & (1U << ((win_base - 0x10) / 8)))) return bus_float + off;
    if (!(regs[socket][win_base + 5] & 0x40)) {
        unsigned offset = regs[socket][win_base + 4] |
                          ((regs[socket][win_base + 5] & 0x3F) << 8);
        assert(((offset + (seg >> 8)) & 0x3FFF) == 0);
        common_reads++;
        return common_memory[socket] + off;
    }
    return memory[socket] + off;
}

static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    memset(selectors, 0, sizeof(selectors));
    memset(memory, 0xFF, sizeof(memory));
    memset(common_memory, 0xFF, sizeof(common_memory));
    common_reads = 0;
    memset(bus_float, 0xFF, sizeof(bus_float));
    shadow_host = 0;
    window_seg = 0xD000;
    win_shadowed = 0;
    reads = writes = live_mapping_writes = 0;
    output_len = 0;
    output[0] = 0;
    pcic = PCIC_BASE;
    sockoff = 0;
    rd_stride = 2;
    rd_attr = 1;
    want_vpp = 0;
    /* Direct decoder tests begin with a simulated active attribute mapping. */
    win_base = 0x10;
    regs[0][6] = 1;
    regs[0][win_base + 4] = 0x30;
    regs[0][win_base + 5] = 0x7F;
    reset_summary();
}

static void expect(const char *s)
{
    if (!strstr(output, s)) {
        fprintf(stderr, "Missing output: %s\nActual:\n%s", s, output);
        abort();
    }
}

static void load_cis(const unsigned char *bytes, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) memory[0][i * rd_stride] = bytes[i];
}

static void test_windows(void)
{
    unsigned char saved[sizeof(regs)];
    int window, powered, i;
    for (window = 0; window < 5; window++) {
        for (powered = 0; powered < 2; powered++) {
            reset();
            regs[0][1] = powered ? 0x4C : 0x0C;
            regs[0][2] = powered ? 0x90 : 0;
            regs[0][3] = powered ? 0x65 : 0;
            regs[0][6] = (unsigned char)(0xDF & ~(1U << window));
            if (powered)
                for (i = 0; i < 6; i++)
                    regs[0][0x10 + window * 8 + i] = (unsigned char)(i * 17 + 3);
            memcpy(saved, regs, sizeof(regs));
            memory[0][0] = 1;
            assert(mapwin(0xD000) == 1);
            assert(win_base == 0x10 + window * 8);
            assert(we_powered == !powered);
            unmapwin();
            assert(live_mapping_writes == 0);
            assert(memcmp(saved, regs, sizeof(regs)) == 0);
        }
    }
    for (powered = 0; powered < 2; powered++) {
        reset();
        regs[0][1] = powered ? 0x4C : 0x0C;
        regs[0][6] = 0xDF;
        memcpy(saved, regs, sizeof(regs));
        assert(mapwin(0xD000) == -1);
        assert(writes == 0);
        assert(memcmp(saved, regs, sizeof(regs)) == 0);
    }
    reset();
    assert(mapwin(0xD000) == 0);
    assert(writes == 0);
}

static void test_irq(void)
{
    const unsigned char entry[] = {0x41, 0x18, 0xAA, 0x60, 0, 2, 7, 0x30, 0, 0x8C};
    const unsigned char cis[] = {
        0x1B, 10, 0x41, 0x18, 0xAA, 0x60, 0, 2, 7, 0x30, 0, 0x8C,
        0x14, 0, 0xFF
    };
    reset();
    memcpy(body, entry, sizeof(entry));
    blen = sizeof(entry);
    parse_cftable(1);
    assert(s_have_irq && s_irq == 0x8C00U);
    expect("mask 0x8C00 { 10 11 15 }");
    body[8] = body[9] = 0xFF;
    reset_summary();
    parse_cftable(1);
    assert(s_have_irq && s_irq == 0xFFFFU);
    expect("mask 0xFFFF {");
    body[7] = 0x2F;
    blen = 8;
    reset_summary();
    parse_cftable(1);
    assert(s_have_irq && s_irq == 0x8000U);
    expect("15 (fixed)");
    body[7] = 0x30;
    reset_summary();
    parse_cftable(1);
    assert(!s_have_irq); /* Truncated mask must not acquire a value. */
    /* Enforce unsigned storage even on a host with 32-bit int. */
    s_irq = (unsigned)-1;
    assert(s_irq > 0);
    reset();
    load_cis(cis, sizeof(cis));
    dumpcis(0xD000, 1);
    expect("SUMMARY: cfg#1 | I/O 0x200..0x207 | IRQmask 0x8C00");
}

static void test_config(void)
{
    int i;
    reset();
    body[0] = 0x10;
    body[1] = 1;
    body[2] = 0x20;
    for (i = 0; i < 5; i++) body[3 + i] = (unsigned char)(i + 1);
    blen = 8;
    parse_config(1);
    assert(s_cor == 0x20);
    expect("reg-present mask: 01 02 03 04 05");
    reset_summary();
    blen = 7;
    parse_config(1);
    assert(s_cor == -1 && s_def_idx == -1);
    expect("truncated CONFIG");
    body[0] = 0x3C;
    for (i = 0; i < 16; i++) body[3 + i] = (unsigned char)(i + 1);
    blen = 19;
    parse_config(1);
    expect("0D 0E 0F 10");
    reset_summary();
    body[0] = 3; /* Four address bytes, but only one provided. */
    blen = 3;
    parse_config(1);
    assert(s_cor == -1);
    blen = 1;
    parse_config(1);
    assert(s_cor == -1);
}

static void test_continuations(void)
{
    unsigned char cis[] = {0x11, 4, 0, 1, 0, 0, 0xFF};
    const unsigned char no_link[] = {0x14, 0, 0xFF};
    unsigned char link_end[259];
    const unsigned char codes[] = {0x11, 0x12, 0x06, 0x03};
    const char *names[] = {"LONGLINK_A", "LONGLINK_C", "LONGLINK_MFC", "INDIRECT"};
    size_t i;
    for (i = 0; i < sizeof(codes); i++) {
        reset();
        cis[0] = codes[i];
        load_cis(cis, sizeof(cis));
        assert(classify(0xD000) == WIN_CIS);
        dumpcis(0xD000, 1);
        expect(names[i]);
        expect("dump is incomplete");
        expect("SUMMARY (partial):");
    }
    reset();
    load_cis(no_link, sizeof(no_link));
    dumpcis(0xD000, 1);
    expect("SUMMARY:");
    assert(!strstr(output, "partial"));
    reset();
    dumpcis(0xD000, 1);
    assert(!strstr(output, "implicit COMMON-memory continuation"));
    reset();
    memset(link_end, 0, sizeof(link_end));
    link_end[0] = 0x20;
    link_end[1] = 0xFF;
    link_end[2] = 0x05; link_end[3] = 0x01;
    link_end[4] = 0x00; link_end[5] = 0x01;
    link_end[257] = 0x21; link_end[258] = 1; /* Must not visit this tuple. */
    load_cis(link_end, sizeof(link_end));
    dumpcis(0xD000, 1);
    assert(s_have_manf && s_manf == 0x0105 && s_prod == 0x0100);
    assert(!s_have_func);
    expect("TPL 20 len 255");
    expect("SUMMARY: MANFID 0105/0100");
    reset();
    memset(memory, 0, sizeof(memory));
    dumpcis(0xD000, 1);
    expect("scan limit reached");
    reset();
    memset(memory, 0, sizeof(memory));
    memory[0][1023 * 2] = 1;
    dumpcis(0xD000, 1);
    expect("tuple header exceeds scan limit");
    reset();
    memset(memory, 0, sizeof(memory));
    memory[0][1021 * 2] = 1;
    memory[0][1022 * 2] = 3;
    dumpcis(0xD000, 1);
    expect("tuple body exceeds scan limit");
    reset();
    rd_attr = 0;
    rd_stride = 1;
    regs[0][win_base + 5] &= ~0x40;
    dumpcis(0xD000, 1);
    assert(!strstr(output, "implicit COMMON"));
}

static void test_existing_card(void)
{
    /* CONFIG and CFTABLE entries from the EXP GAME/MIDI G3 capture. */
    const unsigned char cis[] = {
        0x1A, 5, 1, 2, 0, 1, 1,
        0x1B, 8, 0xC1, 1, 8, 0xAA, 0x60, 0, 2, 7,
        0x1B, 7, 2, 8, 0xAA, 0x60, 8, 2, 7,
        0x14, 0, 0xFF
    };
    reset();
    load_cis(cis, sizeof(cis));
    dumpcis(0xD000, 1);
    expect("SUMMARY: COR@attr 0x100 | cfg#1 | I/O 0x200..0x207");
    assert(!strstr(output, "partial") && !s_have_irq);
}

static void test_implicit_common(void)
{
    /* A valid attribute chain without NO_LINK (like the MC-8000). */
    const unsigned char cis[] = {0x20, 4, 5, 1, 0, 1, 0xFF};
    const unsigned char signatures[][5] = {
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {0x13, 3, 'C', 'I', 'S'},
        {0x13, 2, 'C', 'I', 'S'},
        {0x13, 3, 'C', 'I', 'X'},
        {0x12, 3, 'C', 'I', 'S'},
        {0x13, 4, 'C', 'I', 'S'}
    };
    unsigned char before[sizeof(regs)], mapped[sizeof(regs)];
    size_t i;
    int stride, window, found;
    for (window = 0; window < 5; window++) {
        for (stride = 1; stride <= 2; stride++) {
            for (i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++) {
                reset();
                rd_stride = stride;
                regs[0][1] = 0x4C;
                regs[0][2] = 0x90;
                regs[0][3] = 0x65;
                regs[0][6] = (unsigned char)(0xDF & ~(1U << window));
                load_cis(cis, sizeof(cis));
                memcpy(common_memory[0], signatures[i], 5);
                memcpy(before, regs, sizeof(regs));
                assert(mapwin(0xD000) == 1);
                memcpy(mapped, regs, sizeof(regs));
                dumpcis(0xD000, 1);
                assert(memcmp(mapped, regs, sizeof(regs)) == 0);
                assert(rd_stride == stride && rd_attr == 1);
                assert(common_reads == 5 && live_mapping_writes == 0);
                found = (i == 1 || i == 5);
                assert(!!strstr(output, "implicit COMMON-memory continuation found") == found);
                assert(!!strstr(output, "SUMMARY (partial)") == found);
                expect("MANFID 0105/0100");
                /* The next capture must still read the original attribute bytes. */
                assert(cisb(0xD000, 0) == 0x20);
                assert(cisb(0xD000, 2) == 5);
                unmapwin();
                assert(memcmp(before, regs, sizeof(regs)) == 0);
                assert(live_mapping_writes == 0);
            }
        }
    }
    reset();
    memory[0][0] = 0x14;
    memory[0][2] = 0;
    memcpy(common_memory[0], signatures[1], 5);
    dumpcis(0xD000, 1);
    assert(common_reads == 0); /* NO_LINK suppresses the implicit probe. */
    assert(!strstr(output, "partial"));
}

static void check_capture(unsigned socket)
{
    FILE *f = fopen("CAP.BIN", "rb");
    int i;
    assert(f);
    for (i = 0; i < 512; i++) assert(fgetc(f) == memory[socket][i * 2]);
    assert(fgetc(f) == EOF);
    fclose(f);
    assert(remove("CAP.BIN") == 0);
}

static void test_cli(void)
{
    char *no_socket[] = {"CISDUMP", "/FULL", "/BIN", "CAP.BIN"};
    char *bad_socket[] = {"CISDUMP", "/BIN", "CAP.BIN", "/S", "oops"};
    char *invalid[] = {"-1", "8", "", "1junk", "99999999999999999999999"};
    char *selected[] = {"CISDUMP", "/BIN", "CAP.BIN", "/S", "3"};
    const unsigned char cis[] = {0x20, 4, 5, 1, 0, 1, 0xFF};
    unsigned char saved[sizeof(regs)];
    int i;
    reset();
    assert(cisdump_main(5, bad_socket) == 1);
    assert(reads == 0 && writes == 0);
    for (i = 0; i < (int)(sizeof(invalid) / sizeof(invalid[0])); i++) {
        reset();
        bad_socket[4] = invalid[i];
        assert(cisdump_main(5, bad_socket) == 1);
        assert(reads == 0 && writes == 0);
    }
    reset();
    assert(cisdump_main(4, bad_socket) == 1); /* Missing /S argument. */
    assert(reads == 0 && writes == 0);
    reset();
    regs[3][0] = 0x83;
    regs[3][1] = 0x4C;
    regs[3][2] = 0x90;
    load_cis(cis, sizeof(cis));
    memcpy(memory[3], memory[0], sizeof(memory[0]));
    assert(cisdump_main(5, selected) == 0);
    check_capture(3);
    assert(live_mapping_writes == 0 && regs[3][6] == 0);
    regs[3][6] = 0xDF;
    assert(cisdump_main(5, selected) == 1);
    expect("no free memory window; socket left untouched");

    /* Socket discovery works when the sole card is on the higher bridge. */
    reset();
    regs[4][0] = 0x83;
    regs[4][1] = 0x4C;
    regs[4][2] = 0x90;
    load_cis(cis, sizeof(cis));
    memcpy(memory[4], memory[0], sizeof(memory[0]));
    memcpy(saved, regs, sizeof(regs));
    assert(cisdump_main(4, no_socket) == 0);
    check_capture(4);
    assert(memcmp(saved, regs, sizeof(regs)) == 0);
    assert(live_mapping_writes == 0);

    /* Distinct second card must not replace the first capture. */
    regs[6][0] = 0x83;
    regs[6][1] = 0x4C;
    regs[6][2] = 0x90;
    memcpy(memory[6], memory[4], sizeof(memory[4]));
    memory[6][4] = 0x77;
    memcpy(saved, regs, sizeof(regs));
    assert(cisdump_main(4, no_socket) == 1);
    expect("already used for socket 4; refusing capture from socket 6");
    check_capture(4);
    assert(memcmp(saved, regs, sizeof(regs)) == 0);
    assert(live_mapping_writes == 0);
}

static void test_shadow(void)
{
    char *argv[] = {"CISDUMP", "/FULL", "/S", "0"};
    char *forced[] = {"CISDUMP", "/FULL", "/S", "0", "/FORCE"};
    unsigned char saved[sizeof(regs)];
    unsigned i;
    /* Host RAM at the window: same bytes before and after enable. */
    reset();
    shadow_host = 1;
    for (i = 0; i < sizeof(host_ram); i++) host_ram[i] = (unsigned char)(0x10 + i * 7);
    regs[0][0] = 0x83; regs[0][1] = 0x4C; regs[0][2] = 0x90; regs[0][3] = 0x65;
    memcpy(saved, regs, sizeof(regs));
    assert(cisdump_main(4, argv) == 1);
    expect("shadowing");
    assert(!strstr(output, "TPL "));
    assert(win_shadowed);
    assert(memcmp(saved, regs, sizeof(regs)) == 0);
    assert(live_mapping_writes == 0);
    reset();
    shadow_host = 1;
    regs[0][0] = 0x83; regs[0][1] = 0x4C; regs[0][2] = 0x90; regs[0][3] = 0x65;
    cisdump_main(5, forced);
    expect("shadowing");
    expect("TPL ");
    /* A real card differs from the floating bus. */
    reset();
    regs[0][1] = 0x0C;
    memory[0][0] = 1;
    assert(mapwin(0xD000) == 1 && !win_shadowed);
    unmapwin();
    /* Blank attribute memory reads FF both ways: open bus, not RAM. */
    reset();
    regs[0][1] = 0x0C;
    assert(mapwin(0xD000) == 1 && !win_shadowed);
    unmapwin();
}

static void test_seg(void)
{
    char *bad[] = {"CISDUMP", "/S", "0", "/SEG", "x"};
    char *invalid[] = {"D0", "D080", "F000", "B000", "ED00", "zz", ""};
    char *good[] = {"CISDUMP", "/FULL", "/S", "0", "/SEG", "dc00"};
    char *plain[] = {"CISDUMP", "/S", "0"};
    const unsigned char cis[] = {0x14, 0, 0xFF};
    unsigned i, offset;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset();
        bad[4] = invalid[i];
        assert(cisdump_main(5, bad) == 1);
        assert(reads == 0 && writes == 0);
    }
    reset();
    assert(cisdump_main(4, bad) == 1);      /* missing argument */
    assert(reads == 0 && writes == 0);
    /* The window lands on the requested segment. */
    reset();
    window_seg = 0xDC00;
    regs[0][1] = 0x0C;
    load_cis(cis, sizeof(cis));
    assert(mapwin(0xDC00) == 1);
    assert(regs[0][win_base + 0] == 0xDC && regs[0][win_base + 2] == 0xDF);
    offset = regs[0][win_base + 4] | ((regs[0][win_base + 5] & 0x3F) << 8);
    assert(((offset + 0xDC) & 0x3FFF) == 0);
    unmapwin();
    reset();
    window_seg = 0xDC00;
    regs[0][0] = 0x83; regs[0][1] = 0x0C;
    load_cis(cis, sizeof(cis));
    assert(cisdump_main(6, good) == 0);
    expect("SUMMARY:");
    /* An enabled window on the range, on either socket of the chip, is refused. */
    reset();
    regs[0][1] = 0x0C;
    regs[0][6] = 0x02;                       /* window 1 live at D0-D3 */
    regs[0][0x18] = 0xD0; regs[0][0x1A] = 0xD3;
    assert(mapwin(0xD000) == -2 && writes == 0);
    reset();
    regs[0][1] = 0x0C;
    regs[1][6] = 0x01;                       /* other socket, window 0 at D2-D5 */
    regs[1][0x10] = 0xD2; regs[1][0x12] = 0xD5;
    assert(mapwin(0xD000) == -2 && writes == 0);
    reset();
    regs[0][1] = 0x0C;
    regs[0][6] = 0x02;                       /* window 1 live at DC-DF: no overlap */
    regs[0][0x18] = 0xDC; regs[0][0x1A] = 0xDF;
    memory[0][0] = 1;
    assert(mapwin(0xD000) == 1 && win_base == 0x10);
    unmapwin();
    reset();
    regs[0][0] = 0x83; regs[0][1] = 0x0C;
    regs[0][6] = 0x02; regs[0][0x18] = 0xD0; regs[0][0x1A] = 0xD3;
    assert(cisdump_main(3, plain) == 1);
    expect("already covers");
    assert(writes == 0);
}

int main(void)
{
    test_windows();
    test_irq();
    test_config();
    test_continuations();
    test_existing_card();
    test_implicit_common();
    test_cli();
    test_shadow();
    test_seg();
    puts("PASS: window preservation, IRQ masks, CONFIG, continuations, /BIN, shadow, /SEG");
    return 0;
}
