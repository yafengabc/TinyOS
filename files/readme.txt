TinyOS
======

Welcome to a very small 32-bit x86 operating system.

The kernel boots from a raw disk image with its own boot sector, sets up
paging (4 MiB identity pages for the kernel, 4 KiB pages for user
programs), enters ring 3 and runs /bin/sh as an ELF32 executable that was
loaded from the in-memory ramdisk.

Try:

  ls
  cat readme.txt
  hello one two three
  mem
  crash
  run uptime

Every command other than the shell builtins is a separate ELF binary that
the kernel loads on demand.
