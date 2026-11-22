/* cover.c - one script that exercises the C Script subset end to end.
 *
 *   csc /cover.c /cover.rir     compile
 *   rvm /cover.rir              interpret
 *   rcc /cover.rir /bin/cover   native
 *   cover                       must print the same thing as rvm
 *
 * Everything is split into small functions: the on-board csc has 256 AST
 * nodes and 256 IR per function, so one big main() would not compile there.
 * The output is deliberately fixed text plus the values the checks compute;
 * tests/onboard/cover.expected is the oracle.
 */

struct point {
    int x;
    int y;
};

int fib(int n) {
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int array_sum() {
    int a[6];
    int i;
    int s;

    for (i = 0; i < 6; i = i + 1)
        a[i] = i * i + 1;
    s = 0;
    for (i = 0; i < 6; i = i + 1)
        s = s + a[i];
    return s;
}

int struct_sum() {
    struct point p;

    p.x = 7;
    p.y = 35;
    return p.x + p.y;
}

int while_sum() {
    int n;
    int s;

    n = 1;
    s = 0;
    while (n <= 10) {
        s = s + n;
        n = n + 1;
    }
    return s;
}

int main() {
    int v;
    string s;
    float f;

    printf("cover: start\n");

    v = fib(10);
    printf("fib=%d\n", v);
    v = array_sum();
    printf("array=%d\n", v);
    v = struct_sum();
    printf("struct=%d\n", v);
    v = while_sum();
    printf("while=%d\n", v);

    s = "hello";
    v = strlen(s);
    printf("strlen=%d\n", v);
    v = strcmp(s, "hello");
    printf("strcmp=%d\n", v);

    f = 1.5;
    f = f * 2.0;
    printf("float=%.2f\n", f);

    printf("cover: ok\n");
    return 0;
}
