#include "lib.h"

/* Control program: the same output as "hello", but written with printf.

   It exists to keep lib.c's printf exercised by the smoke test and to give
   a size reference: printf adds ~960 bytes of .text, while the puts/putint
   version of hello stays around 1.2 KB.  compare:

       hello         puts + putint   ~1.2 KB
       hello_printf  printf          ~2.0 KB
*/

int main(int argc, char *argv[])
{
    printf("hello from ring 3!\n");
    printf("pid=%d argc=%d uptime=%d ms\n", getpid(), argc, uptime());
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
