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

Try:

  ls
  cat readme.txt
  hello one two three
  mem
  crash
  run uptime

Every command other than the shell builtins is a separate ELF binary that
the kernel loads from the filesystem when you type its name.
