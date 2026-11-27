/* help - the command map, for both shells.
 *
 * This was a sh builtin: 2.6 KiB of strings that msh could not print at all
 * because it has no room for them.  As an applet both shells reach it through
 * spawn()'s /bin/tools retry, `help > file` writes the text like any other
 * command, and sh's image lost the strings.
 *
 * The applet list is printed from tools/applets.h so it cannot go stale.
 */
#include "lib.h"
#include "tools/applets.h"

int applet_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    puts("TinyOS help\n\n");
    puts("Shell builtins (sh and msh):\n");
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
    puts("  ls edit reboot gpio adc spi i2c pwm uart fb screen splash\n");
    puts("  lcdconsole smp ping host net http dhcp wificonn wifiscan ...\n");
    puts("\nFiles, devices and pipes:\n");
    puts("  cat /proc/ps       the task table as a file\n");
    puts("  echo 1 > /dev/gpio4  pins and ADC channels are files: write to\n");
    puts("                     drive, cat to read, `gpio dev` for ioctl\n");
    puts("  cmd > file         send stdout to a file or a device\n");
    puts("  A | B              send A's stdout to B's stdin\n");
    puts("  run sh <file>      run a script as a task\n");
    puts("\nScript syntax (sh only; msh runs programs, pipes and variables):\n");
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
