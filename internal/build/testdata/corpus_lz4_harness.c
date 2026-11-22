/* corpus_lz4_harness.c - run the kernel's LZ4 block decoder over one corpus
 * file and print what it produced.
 *
 * The Go side (internal/build/lz4.go) and the kernel side (kernel/lz4.c) are
 * two implementations of the same format.  tests/corpus/lz4/ holds the bytes
 * both must agree on; this harness is the C half of that agreement.  Built by
 * internal/build/corpus_test.go with the host compiler, next to the real
 * kernel/lz4.c and kernel/string.c - not a copy of them.
 *
 *     corpus_lz4_harness input.bin
 *     accept 68656c6c6f      or      reject
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lz4.h"

typedef struct {
    const unsigned char *p;
    size_t n, off;
} mem_t;

static int mem_read(void *ctx, uint8_t *buf, uint32_t cap)
{
    mem_t *m = (mem_t *)ctx;
    size_t left = m->n - m->off;
    size_t take = left < (size_t)cap ? left : (size_t)cap;

    memcpy(buf, m->p + m->off, take);
    m->off += take;
    return (int)take;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s input.bin\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        return 2;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        return 2;
    }
    unsigned char *in = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (!in) {
        return 2;
    }
    if (n > 0 && fread(in, 1, (size_t)n, f) != (size_t)n) {
        return 2;
    }
    fclose(f);

    static uint8_t dst[65536];
    mem_t src = {in, (size_t)n, 0};
    lz4_stream_t s;
    memset(&s, 0, sizeof s);
    s.read = mem_read;
    s.ctx = &src;

    int got = lz4_stream_decompress(&s, dst, (uint32_t)sizeof dst);
    free(in);
    if (got < 0) {
        printf("reject\n");
        return 0;
    }
    printf("accept ");
    for (int i = 0; i < got; i++) {
        printf("%02x", dst[i]);
    }
    printf("\n");
    return 0;
}
