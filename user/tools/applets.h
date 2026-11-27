/* The applets inside /bin/tools.
 *
 * Three users: tools/dispatch.c (the function table), ls.c (the
 * `du -> tools` line in the long listing) and help.c (the applet list it
 * prints).  The shells do not need the list: spawn() retries /bin/tools after
 * a failed exec and argv[0] selects the applet.  Keep it an X-macro so adding
 * one cannot leave a table behind.
 */
#ifndef TOS_TOOLS_APPLETS_H
#define TOS_TOOLS_APPLETS_H

#define TOS_APPLETS(X) \
    X(cat) X(cp) X(dd) X(df) X(du) X(echo) X(flashinfo) X(free) X(grep) \
    X(head) X(kill) X(mkdir) X(rmdir) X(stat) X(tail) X(tee) \
    X(uptime) X(wc) X(wfile)

#endif
