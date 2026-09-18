#ifndef TEST_DOS_H
#define TEST_DOS_H
#define __far
unsigned char *test_mk_fp(unsigned seg, unsigned off);
#define MK_FP(seg, off) test_mk_fp((seg), (off))
#endif
