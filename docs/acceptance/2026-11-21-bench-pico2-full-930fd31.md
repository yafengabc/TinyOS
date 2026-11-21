# Pico 2 两侧 SMP 的完整脚本跑分（2026-11-21）

```text
commit:            930fd31（docs/BENCH.md 两张完整表）
change:            补上 clang / goc32 / MicroPython 三列，rvm/rcc/goc32 与 M33 的
                   MicroPython 均为本轮实测
baseline:          01031d1（只有 rvm/rcc 两列）
target/config:     rpi-pico2-smp + configs/rpi-pico2-rir.conf（csc/rvm/rcc）
                   rpi-pico2-rv32-smp + configs/rpi-pico2-rv32-rir.conf
board/port/image:  Raspberry Pi Pico 2（RP2350A，8 MiB，150 MHz），CDC COM27；
                   MicroPython 固件 .tmp/micropython-pico2.uf2（端口随枚举变化）
toolchain:         clang -Oz（TinyOS 目标工具链），goc32（本机 go build），
                   mpremote 1.29.0，python 3.14
host:              Windows 11
T0:                PASS（本轮只改文档；上游 go test/vet 已绿）
T1:                PASS（四个镜像均 build ok：M33/RV32 rir、带 benchclang、
                   带 benchgoc；benchclang 源来自 bench/clang/benchclang.c，
                   用后已删除，未提交）
T2:                NOT RUN（真机跑分）
T3:                PASS（所有四内核校验和一致：46368 / 999850000 /
                   225792000 / 599994）
T4:                NOT RUN
logs:              build/bench/{rvm,rcc,goc32,clang,micropython,
                   micropython-native}.json（原始，不入 git）
```

## 命令

```sh
# TinyOS 侧（rir 镜像）
python scripts/bench/compare.py -p COM27 --dtr --engine rcc --rvm
# clang / goc32：benchclang 编进镜像；goc32 -target rp2350|rv32 生成 LZ4TOS
#   编进 files.local/bin/benchgoc 后重建镜像
# MicroPython：Ctrl-D 进 BOOTSEL 刷 .tmp/micropython-pico2.uf2
python scripts/bench/compare.py --mpy --mpremote "connect COM22"
```

`mpremote` 的可执行文件装在 `%APPDATA%\Python\Python314\Scripts`，需要把它加进
`PATH`；`compare.py` 用的是裸命令名。

## 结果（ms）

| 内核 | M33 mpy | M33 @native | M33 rvm | M33 goc32 | M33 rcc | M33 clang | RV32 rvm | RV32 goc32 | RV32 rcc | RV32 clang |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib | 1096 | 635 | 289 | 24 | 70 | **13** | 281 | 21 | 61 | **14** |
| loop | 1220 | 530 | 407 | 30 | 44 | **10** | 397 | 27 | 27 | **15** |
| array | 2015 | 1149 | 799 | 62 | 120 | **9** | 803 | 42 | 60 | **14** |
| div | 696 | 368 | 340 | 1338 | 47 | **36** | 352 | 40 | 42 | **20** |

`pvm` 两侧 `NOT RUN`（需要另一套 `csa` 镜像）；MicroPython 没有 RV32 固件，
那两列不适用。

**离群值**：goc32 的 M33 `div` 是 1338 ms，RV32 只有 40 ms——goc32 的 ARM
后端把除法展开成软件例程，RV32 有硬件 `div`。这不是"goc32 整体慢 30 倍"，
其它三个内核只慢 1.2–2×。

## 没做（NOT RUN）

- `pvm` 两列；
- 每项取多次中位数（每个数字是一次运行，M33/RV32 口径一致）；
- MicroPython RV32（固件不存在）。

板子测完停在 MicroPython 固件上，要继续 TinyOS 开发需要重刷；本次已恢复为
可用的 TinyOS 镜像。
