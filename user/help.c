/* help - the command map.
 *
 * A program of its own, not a tools applet: the text is 2.6 KiB and it used
 * to drag the multi-call binary into the next window class (24 KiB span
 * instead of 20 on rv32).  As /bin/help only `help` pays for it: 4 KiB span,
 * a 12 KiB window.  Both shells find it by name; `help > f` works like any
 * other program.
 *
 * The applet list is printed from tools/applets.h so it cannot go stale.
 */
#include "lib.h"
#include "tools/applets.h"

/* Prints strings and exits: no heap, no depth.  4 KiB, no grow reserve. */
TOS_STACK(4 * 1024);

int main(void)
{
    puts("TinyOS help\n\n");
    puts("Shell builtins (sh):\n");
    puts("  cd [dir]           change directory (default /); pwd prints it\n");
    puts("  echo [args...]     print arguments\n");
    puts("  history            list recent commands (up/down recalls them)\n");
    puts("  ps                 list tasks (also /proc/ps, /proc/mem, /proc/uptime)\n");
    puts("  sleep <ms>         wait without using the CPU\n");
    puts("  rm <file>...       remove files\n");
    puts("  run <prog> [args] [&]   run a program (cwd, /bin, then /bin/tools)\n");
    puts("  nice <n> <prog> .. run at priority n (0 high, 2 low)\n");
    puts("  prio <pid> <n>     move a running task to priority n\n");
    puts("  source <file>      run a script in this shell\n");
    puts("  true / false       set $? to 0 / 1\n");
    puts("  exit [code]        shut TinyOS down (end the script, in a script)\n");
    puts("\nTools in /bin/tools (also `tools <name>`):\n ");
#define X(n) puts(" " #n);
    TOS_APPLETS(X)
#undef X
    puts("\n\nPrograms in /bin:\n");
    puts("  ls edit help top reboot gpio adc spi i2c pwm uart fb screen splash\n");
    puts("  lcdconsole smp ping host net http dhcp wificonn wifiscan ...\n");
    puts("  msh is frozen and not in a default image (sh is the shell)\n");
    puts("\nFiles, devices and pipes:\n");
    puts("  cat /proc/ps       the task table as a file\n");
    puts("  echo 1 > /dev/gpio4  pins and ADC channels are files: write to\n");
    puts("                     drive, cat to read, `gpio dev` for ioctl\n");
    puts("  cmd > file         send stdout to a file or a device\n");
    puts("  A | B              send A's stdout to B's stdin\n");
    puts("  run sh <file>      run a script as a task\n");
    puts("\nScript syntax (sh):\n");
    puts("  name=text          set a variable (also `name=$(prog args)`)\n");
    puts("  incr name [n]      add n (default 1) to an integer variable\n");
    puts("  $name  ${name}  $? text substitution; $(prog) captures stdout\n");
    puts("  if <a> == <b>      also != -eq -ne -lt -gt -le -ge, -n a, -z a\n");
    puts("  else / fi          optional else branch / end of if\n");
    puts("  while <cond> ... done   loop; break leaves the innermost loop\n");
    puts("\nLine editing: ^A/^E home/end, ^U kill line, ^K kill to end,\n");
    puts("  ^W kill word, arrows move/history, Backspace, Delete, Tab=space.\n");
    return 0;
}
