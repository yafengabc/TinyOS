TinyOS
======

Welcome to a very small multi-platform operating system.

There is no MMU and no paging: the kernel runs in a single flat address
space, gives every task its own window of RAM and validates user pointers
against it.  The same ELF32 binaries run on the PC port (boot sector, ATA
disk) and on the RV32 port (QEMU virt, RAM disk) because programs are
linked as position independent executables and relocated on load.

/bin/sh is itself an ELF32 executable loaded from littlefs, and it starts
every program below on demand.

Naming a task's RAM is all the isolation there is: no paging, no MMU.  The
RV32 port can add one more layer because RISC-V checks every user access
against PMP, so a task's code, data and stack are the only memory it can
reach - see "prot rodata|heap|stack|null", which breaks one rule per run
and gets the task killed instead of the machine.

Try:

  ls
  cat readme.txt
  hello one two three
  mem
  crash
  run uptime
  run spin &          run a program in the background
  ps                  show the task table

exec only starts a task; the shell waits for it, so a foreground command
behaves as before.  Programs that never give up the CPU are preempted
every 50 ms, which is what makes "run spin &" keep the shell usable.

Every command other than the shell builtins is a separate ELF binary that
the kernel loads from the filesystem when you type its name.
