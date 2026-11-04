/* py_main.c - the interpreter's entry point.
 *
 *     py script.py        run a file
 *     py -c "print(1)"    run a line
 *
 * The interpreter needs an arena: the token array, the bytecode, the
 * name and constant pools and every object the script builds live in it.
 * The arena starts as one sbrk() of the heap
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
#include "py_lex.h"
#include "py_code.h"
#include "py_rt.h"
#include "py_str.h"

/* How much heap the interpreter wants, and how much stack.
 *
 * The kernel reads both notes before it sizes the task window, so the
 * window is big enough for the arena and the recursion even though the
 * interpreter's own image is small.
 *
 * The heap is what a script costs: the token array, the bytecode, the
 * name and constant pools and the objects the script builds all live in
 * the interpreter's arena.  Measured on the ESP32-C3, a hundred-line
 * script needs 60 KiB of it including headers and rounding.  The arena
 * grows on demand from a 32 KiB start, so a small script does not pin
 * the break at the heap ceiling (see py_mem.c).
 *
 * The stack is 16 KiB: the compiler is a recursive-descent walk over the
 * expression, and the VM recurses once per Python call.  The board
 * default of 8 KiB is enough for a flat script and not for a deep one.
 *
 * Together with the image and the guard hole these make a 128 KiB window
 * on the ESP32-C3, whose whole user area is 192 KiB.  The interpreter is
 * the first program on this system that needs a window of that size; the
 * window goes back to the area when the script ends. */
TOS_HEAP(64 * 1024);
TOS_STACK(16 * 1024);

/* The script, kept so an error can quote the line it happened on. */
static const char *src_text;
static uint32_t src_len;
static const char *src_name;

/* Take a starting chunk for the arena.
 *
 * Deliberately small: the arena grows on demand (see py_mem.c), so a
 * one-line script does not have to pin the whole heap.  Leaving the
 * break below the heap ceiling is what lets the kernel grow this
 * task's stack if a deep expression needs it.  A refused sbrk() leaves
 * the break alone, so stepping down costs nothing. */
static uint32_t claim_heap(void **base_out)
{
    for (uint32_t size = 32u * 1024u; size >= 4096u; size -= 4096u) {
        void *old = sbrk((int)size);
        if (old) {
            *base_out = old;
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

    /* The source becomes tokens, the tokens become bytecode, and the
     * bytecode runs.  The tokens are the only thing that exists in
     * between: there is no tree, so a script that used to cost forty
     * kilobytes of syntax tree costs a couple of kilobytes of code. */
    py_tokens toks;
    toks.toks = 0;
    toks.n = 0;
    toks.cap = 0;
    if (!py_lex_all(code, len, &toks)) {
        printf("  File \"%s\", line %u\n", name, py_lex_err_line());
        printf("SyntaxError: %s\n",
               py_lex_err_msg() ? py_lex_err_msg() : "invalid syntax");
        return 1;
    }

    py_prog *prog = py_compile(&toks);
    if (!prog) {
        printf("  File \"%s\", line %u\n", name, py_compile_error_line());
        printf("SyntaxError: %s\n",
               py_compile_error() ? py_compile_error() : "invalid syntax");
        return 1;
    }

    if (verbose) {
        printf("bytecode: %u instructions, %u B (%u code objects)\n",
               py_prog_insns(prog), py_prog_bytes(prog), prog->ncodes);
        printf("arena after compile: %u B (peak %u B, %u blocks)\n",
               py_mem_used(), py_mem_peak(), py_mem_live_blocks());
    }

    int rc = 0;
    if (py_vm_run(prog) != 0) {
        printf("Traceback (most recent call last):\n");
        printf("  File \"%s\", line %u\n", name, py_vm_error_line());
        show_line(py_vm_error_line());
        printf("%s\n", py_vm_error());
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
