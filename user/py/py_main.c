/* py_main.c - the interpreter's entry point.
 *
 *     py script.py        run a file
 *     py -c "print(1)"    run a line
 *
 * The interpreter needs an arena: the syntax tree, the token list and every
 * object the script builds live in it.  The arena is one sbrk() of the heap
 * the kernel reserved for this task, which is what the TOS_HEAP note below
 * asks for - the kernel reads the note before it sizes the task window, so
 * the room is reserved rather than hunted for.
 *
 * The user libc's malloc() cannot be used for this: it is a bump allocator
 * whose free() does nothing, and it grows the break 4 KiB at a time.  An
 * interpreter allocates thousands of small short-lived objects, so it needs
 * an allocator that gives memory back.  See py_mem.h for why this is not
 * TLSF.
 */

#include "lib.h"
#include "py_mem.h"
#include "py_obj.h"
#include "py_parse.h"
#include "py_eval.h"
#include "py_str.h"

/* How much heap the interpreter wants, and how much stack.
 *
 * The kernel reads both notes before it sizes the task window, so the
 * window is big enough for the arena and the recursion even though the
 * interpreter's own image is small.
 *
 * The heap is what a script costs: the syntax tree, the token list, the
 * text of the names and the objects the script builds all live in the
 * interpreter's arena.  Measured on the ESP32-C3: a hundred-line script
 * takes about 83 KiB of it, headers and rounding included, so 96 KiB holds
 * a script of that size with room to spare.
 *
 * The stack is 16 KiB because the evaluator recurses once per Python call
 * and once per node of the tree, and the parser recurses once per level of
 * nesting.  The board default of 8 KiB is enough for a flat script and
 * not for a deep one.
 *
 * Together with the image and the guard hole these make a 128 KiB window
 * on the ESP32-C3, whose whole user area is 192 KiB.  The interpreter is
 * the first program on this system that needs a window of that size; the
 * window goes back to the area when the script ends. */
TOS_HEAP(96 * 1024);
TOS_STACK(16 * 1024);

/* The script, kept so an error can quote the line it happened on. */
static const char *src_text;
static uint32_t src_len;
static const char *src_name;

/* Claim the task's heap for the interpreter's arena.
 *
 * The kernel does not tell a program how big its heap is, so the size is
 * found by asking for more than there is: sbrk() refuses and leaves the
 * break alone, and a smaller request succeeds.  The first size that fits is
 * the one taken, and it is taken in one piece - the arena is a single
 * region, not a series of 4 KiB steps. */
static uint32_t claim_heap(void **base_out)
{
    void *base = sbrk(0);
    if (!base)
        return 0;

    /* Steps of 4 KiB, from generous to tight.  A coarse ladder would leave
     * part of the heap unused, and on a board whose heap is the difference
     * between a script that parses and one that does not, every kilobyte
     * counts.  A refused sbrk leaves the break alone, so trying many sizes
     * is free. */
    for (uint32_t size = 128u * 1024u; size >= 4096u; size -= 4096u) {
        void *old = sbrk(size);
        if (old) {
            *base_out = base;
            return size;
        }
    }
    return 0;
}

/* Print the line an error happened on, the way Python's traceback does. */
static void show_line(uint32_t line)
{
    if (!src_text || !line)
        return;

    uint32_t start = 0;
    uint32_t cur = 1;
    while (cur < line && start < src_len) {
        if (src_text[start] == '\n')
            cur++;
        start++;
    }
    uint32_t end = start;
    while (end < src_len && src_text[end] != '\n')
        end++;

    write(2, "    ", 4);
    write(2, src_text + start, (int)(end - start));
    write(2, "\n", 1);
}

/* Read a script into the arena.  Returns 0 on success. */
static int load_file(const char *path, char **out, uint32_t *out_len)
{
    int fd = open(path);
    if (fd < 0)
        return -1;

    struct stat st;
    if (stat(path, &st) < 0 || st.type != TOS_FTYPE_REG) {
        close(fd);
        return -1;
    }
    if (st.size == 0 || st.size > 128u * 1024u) {
        close(fd);
        return -2;
    }

    char *buf = (char *)py_alloc(st.size + 1);
    if (!buf) {
        close(fd);
        return -2;
    }
    int n = read(fd, buf, (int)st.size);
    close(fd);
    if (n < 0)
        n = 0;
    buf[n] = 0;

    *out = buf;
    *out_len = (uint32_t)n;
    return 0;
}

int main(int argc, char **argv)
{
    void *base = 0;
    uint32_t size = claim_heap(&base);
    if (!size) {
        printf("py: no heap to run in\n");
        return 1;
    }
    py_mem_init(base, size);

    char *code = 0;
    uint32_t len = 0;
    const char *name = "<stdin>";
    int verbose = 0;

    if (argc >= 2 && !strcmp(argv[1], "-v")) {
        verbose = 1;
        argv++;
        argc--;
    }

    if (argc >= 2 && !strcmp(argv[1], "-c")) {
        if (argc < 3) {
            printf("usage: py [-v] [-c code] [file]\n");
            return 2;
        }
        code = argv[2];
        len = (uint32_t)strlen(code);
        name = "<string>";
    } else if (argc >= 2) {
        name = argv[1];
        int r = load_file(argv[1], &code, &len);
        if (r == -2) {
            printf("py: %s: cannot read (too big, or no room)\n", argv[1]);
            return 1;
        }
        if (r != 0) {
            printf("py: %s: no such file\n", argv[1]);
            return 1;
        }
    } else {
        printf("usage: py [-v] [-c code] [file]\n");
        return 2;
    }

    src_text = code;
    src_len = len;
    src_name = name;

    py_scope *mod = py_parse(code, len);
    if (!mod) {
        printf("  File \"%s\", line %u\n", name, py_err_line());
        printf("SyntaxError: %s\n", py_err_msg() ? py_err_msg() : "invalid syntax");
        if (verbose)
            printf("arena after parse: %u B (peak %u B, %u blocks)\n",
                   py_mem_used(), py_mem_peak(), py_mem_live_blocks());
        return 1;
    }

    if (verbose)
        printf("arena after parse: %u B (peak %u B, %u blocks)\n",
               py_mem_used(), py_mem_peak(), py_mem_live_blocks());

    int rc = 0;
    if (py_run(mod) != 0) {
        printf("Traceback (most recent call last):\n");
        printf("  File \"%s\", line %u\n", name, py_eval_line());
        show_line(py_eval_line());
        printf("%s\n", py_eval_error());
        rc = 1;
    }

    if (verbose) {
        /* The arena is the interpreter's whole memory budget, so it is worth
         * being able to see what it cost. */
        printf("arena: %u B of %u B, peak %u B, %u live blocks\n",
               py_mem_used(), py_mem_total(), py_mem_peak(), py_mem_live_blocks());
        printf("arena used: %u B (headers and rounding included)\n",
               py_mem_arena_used());
    }
    return rc;
}
