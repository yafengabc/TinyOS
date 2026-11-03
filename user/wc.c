#include "lib.h"

/* wc - count lines, words and bytes on standard input.
 *
 * Reads fd 0 until EOF, which is what makes it useful at the end of a
 * pipeline: `cat readme.txt | wc` exercises the pipe in both directions
 * (bytes in through fd 0, the report out through fd 1). */
int main(void)
{
    char buf[128];
    int n;
    unsigned int lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];

            bytes++;
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    if (n < 0) {
        puts("wc: read error\n");
        return 1;
    }

    printf("%d %d %d\n", (int)lines, (int)words, (int)bytes);
    return 0;
}
