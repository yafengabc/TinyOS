/* state.c - cooperative state machines, the shape async code takes when the
 * language has no coroutines.
 *
 * Each job is a small struct (its saved state) plus a step() function that
 * does one unit of work per call.  It returns the updated struct; a state of
 * 2 means "finished".  main() is the event loop: call every unfinished step,
 * then yield once.
 *
 * The struct is passed and returned by value because csc has neither struct
 * pointers nor arrays inside structs; keep the saved state to scalars.  The
 * split by job (rather than one big switch) also matters: csc compiles one
 * function at a time and the on-board build allows 256 AST nodes / 256 IR
 * instructions per function.  A protocol with 30 states should be split into
 * phases (begin/header/body/end), each its own small step function.
 */

struct counter {
    int state;
    int n;
    int total;
};

struct reader {
    int state;
    int pos;
    int sum;
};

struct counter counter_step(struct counter c)
{
    switch (c.state) {
    case 0:
        c.n = 0;
        c.total = 0;
        c.state = 1;
        return c;
    case 1:
        if (c.n >= 5) {
            c.state = 2;
            return c;
        }
        c.total = c.total + c.n;
        c.n = c.n + 1;
        return c;
    }
    return c;
}

struct reader reader_step(struct reader r)
{
    switch (r.state) {
    case 0:
        r.pos = 0;
        r.sum = 0;
        r.state = 1;
        return r;
    case 1:
        if (r.pos >= 6) {
            r.state = 2;
            return r;
        }
        r.sum = r.sum + ('A' + r.pos);
        r.pos = r.pos + 1;
        return r;
    }
    return r;
}

int main()
{
    struct counter c;
    struct reader r;
    int done_c;
    int done_r;
    int turns;

    c.state = 0;
    r.state = 0;
    done_c = 0;
    done_r = 0;
    turns = 0;

    while (done_c == 0 || done_r == 0) {
        if (done_c == 0) {
            c = counter_step(c);
            if (c.state == 2)
                done_c = 1;
        }
        if (done_r == 0) {
            r = reader_step(r);
            if (r.state == 2)
                done_r = 1;
        }
        turns = turns + 1;
        yield();   /* cooperative: no timer quantum */
    }

    printf("turns=%d\n", turns);
    printf("sum=%d\n", c.total);
    printf("readsum=%d\n", r.sum);
    return 0;
}
