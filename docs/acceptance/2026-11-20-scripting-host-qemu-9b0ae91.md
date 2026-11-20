# csc + pvm scripting：host / 18 targets / QEMU（2026-11-20）

```text
commit:            9b0ae91
change:            csc（原 csc2）成为唯一脚本编译器；pvm 是裁剪 AMX core，
                   带字节字符串、Q16.16 float、GPIO/ADC/PWM 和第一批
                   file/process syscall；py/avm/rvm 默认不进镜像
baseline:          76db4c5
target/config:     host + 18 targets；configs/esp32c3-8k.conf 构建检查
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763（MSYS2 UCRT64）
T0:                PASS（go build/vet/test；1 skip：TestLittlefsImageRoundTrip，
                   zig 未安装；gofmt 报的 9 个 rv32emu 文件是既有问题）
T1:                PASS（go run ./cmd/tos build all，18/18）
T2:                PASS（tostest smoke rv32 / rv32smp / arm，三次 SMOKE TEST OK）
T3:                NOT RUN here（见 C3 卡）
T4:                NOT RUN
logs:              build/testlogs/（原始日志本地，不入库）
```

## 脚本栈

- `csc` 窗口 **100 KiB**：span 82 KiB + 16 KiB 编译器栈 + 4 KiB guard；
  `-Oz` + 流式写 `.amx`，ELF 22,918 B，`/bin/csc` 18,217 B。
- `pvm` ELF 14,326 B，`/bin/pvm` 10,715 B；`tos_print`（Pawn）和
  `tos_prints`（csc 字节串）并存。
- `py` 已从 x86 stage 移除（57 个文件，之前 58 个）；`csc` + `pvm` 只在
  `scripting = true` 的镜像里。

## QEMU 脚本

rv32 上 `csc` + `pvm` 跑过：

- `m2` 33/120/7，`m3` 45/20；
- `m4` struct/UFCS/switch/enum/printf，`m5` 复合运算；
- `m9` 字节字符串；`m10` GPIO/ADC/PWM；`m11/m12` Q16.16 float 与复合赋值；
- `m13` `char` 标量/数组/`++`；
- `msys` 文件/目录/时间/进程 syscall：
  `create=3 write=14 open=3 read=14 seek=0 cwd=/ dir=dev pid=3 yield=0 dup=3 ioctl=-1 unlink=0`。

全部 `SMOKE TEST OK`。
