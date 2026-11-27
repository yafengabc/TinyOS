/* The applets inside /bin/tools.
 *
 * Two users: tools/dispatch.c (the function table) and ls.c (the
 * `du -> tools` line in the long listing).  The shells do not need the list:
 * spawn() retries /bin/tools after a failed exec and argv[0] selects the
 * applet.  Keep it an X-macro so adding one cannot leave a table behind.
 */
#ifndef TOS_TOOLS_APPLETS_H
#define TOS_TOOLS_APPLETS_H

#define TOS_APPLETS(X) \
    X(cat) X(cp) X(dd) X(df) X(du) X(echo) X(flashinfo) X(free) X(grep) \
    X(head) X(help) X(kill) X(mkdir) X(rmdir) X(stat) X(tail) X(tee) \
    X(uptime) X(wc) X(wfile)

#endif
