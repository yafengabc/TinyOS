/* ready.c - the two scheduling natives csc exposes to scripts.
 *
 * `ready(fd)` is one non-blocking poll: 1 when the descriptor has something
 * to read, 0 when it does not.  `yield()` gives up the CPU for one round
 * without the 1 ms cost of sleep(1).  A state machine uses them as
 *
 *     if (ready(0)) { ... } else { work; yield(); }
 */
int main() {
    int fd;
    int r;

    fd = open("/readme.txt");
    r = ready(fd);
    printf("ready=%d\n", r);
    close(fd);

    printf("y=%d\n", yield());
    return 0;
}
