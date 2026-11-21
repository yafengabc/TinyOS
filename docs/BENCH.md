# 脚本工具链跑分：MicroPython vs pvm / rvm / rcc

同一组整数内核在三块板、最多七个执行环境上跑，比较**毫秒数**，不看主观感受。
本文先描述方法与脚本，结果是一张总表（见「结果」一节）：ESP32-C3 七列全有，
Pico 2 的 M33 与 RV32 两侧各有五列，缺的显式记 `—` / `NOT RUN`。

> **两套东西，别混：**
> - `bench/bench.c` + `bench/bench.py` 是**计时基准**：4 个内核（fib/loop/array/div），
>   一次跑完只有 4 行 `BENCH`。
> - `tests/*.c`（44 个）是**功能回归语料**，用来看正确性，不是基准。
>   把语料当基准跑出来的表只有 `rcc` 一列，而且没有 array/div 这两个内核——
>   那不是本表，别贴进结果。

## 一条命令（推荐）

镜像里有什么工具，就跑什么引擎，缺的显式记 `NOT RUN`：

```sh
python scripts/bench/compare.py --auto --port COM4
python scripts/bench/compare.py --report
```

`--auto` 先探测 `/bin` 和 `/`：有 csc 就编 `/bench.c`，有 rvm/rcc 就跑，
有 `benchclang`/`benchgoc` 就跑，镜像带 csa/pvm 就跑 amx。`--report` 会把
七个引擎全部列出，没测的写 `NOT RUN` 和原因，避免"只出一列"看起来像完整结果。

哪些引擎在哪个镜像里：

| 引擎 | 需要的镜像/固件 |
|---|---|
| `rvm`, `rcc` | rir 镜像（csc+rvm+rcc），`bench.c` 在根目录 |
| `pvm` | 默认镜像（csa+pvm，无 rir） |
| `clang` | 镜像里含 `/bin/benchclang`（见下节） |
| `goc32` | 镜像里含 `/bin/benchgoc`（见下节） |
| `micropython`, `-native` | 刷 MicroPython 固件（`--mpy`，在宿主跑 mpremote） |

## 被测对象

| 名称 | 是什么 | 怎么跑 |
|---|---|---|
| `micropython` | MicroPython 字节码解释器 | `mpremote run bench/bench.py` |
| `micropython-native` | MicroPython `@micropython.native` | `mpremote run bench/bench_native.py` |
| `pvm` | 旧 AMX 字节码 VM（`csa` 编译） | 板上 `csa /bench.c /b.amx` → `pvm /b.amx` |
| `rvm` | 新 RIR2 解释器（`csc` 编译） | 板上 `csc /bench.c /b.rir` → `rvm /b.rir` |
| `rcc` | 新原生编译器（ARM Thumb-2 / RV32） | 板上 `rcc /b.rir /bin/benchc` → `benchc` |

为什么只测整数：TinyOS 的 `float` 是 **Q16.16**，MicroPython 是 IEEE double，
比浮点等于比两套不同的数学库。整数语义两者一致，`sum=` 可以逐位对照。

## 内核

`bench/bench.c` 与 `bench/bench.py` / `bench_native.py` 是同一组内核：

| 内核 | 内容 | 规模 | 校验和 |
|---|---|---|---|
| `fib` | 递归 `fib(24)` | 约 15 万次调用 | `46368` |
| `loop` | 加法 + 分支，累加器自绕回 | 300 000 次 | `999850000` |
| `array` | 256 字数组读写 | 1000 轮 | `225792000` |
| `div` | 每次一次 `% 7` | 200 000 次 | `599994` |

规模按“最快的引擎（160 MHz C3 上的 rcc）也要跑几十毫秒”选；否则 1 ms 的
`uptime/ticks_ms` 分辨不出来。**校验和不一致的行不能比较**——那说明跑的不是同一
份工作负载（脚本会检查）。

## 步骤

`csa`/`pvm` 与 `csc`/`rvm`/`rcc` 在构建里互斥（`defaultUserExclude`），所以要分镜像：

```sh
# 1) 默认镜像（csa + pvm）
go run ./cmd/tos build -config configs/esp32c3.conf
python scripts/esp-c3/flash.py --full -p COM4
cp bench/bench.c files.local/          # 已经由脚本自动 stage
python scripts/bench/compare.py --port COM4 --engine pvm

# 2) rir 镜像（csc + rvm + rcc）
go run ./cmd/tos build -config configs/esp32c3-rir.conf
python scripts/esp-c3/flash.py --full -p COM4
python scripts/bench/compare.py --port COM4 --engine rcc --rvm

# 3) MicroPython 固件（同一块板）
#    https://micropython.org/download/ESP32_GENERIC_C3/
pip install esptool mpremote
esptool.py --chip esp32c3 -p COM4 erase_flash
esptool.py --chip esp32c3 -p COM4 write_flash 0 ESP32_GENERIC_C3-*.bin
python scripts/bench/compare.py --mpy

# 4) 汇总
python scripts/bench/compare.py --report
```

`compare.py` 把结果写到 `build/bench/<engine>.json`，`--report` 打印 Markdown 表；
`--no-compile` 用于板上已有 `.rir`/`.amx` 的情况。TinyOS 控制台的串口参数与
`scripts/rp2040/thumb1-run.py` 一致（USB CDC 加 `--dtr`）。

## 注意事项

- **镜像不同，板子相同**：三次刷写都在同一块 C3 上，时钟不变，才可以横向比。
- **板上编译时间不计入**：`csa`/`csc`/`rcc` 的编译耗时是另一件事；这里只量运行。
- **rcc 是原生代码，rvm/pvm/MicroPython 是解释器**，差一个数量级是正常的；
  有意义的是 `rvm` 与 `pvm`（都是字节码）、`rcc` 与 `micropython-native`
  （都是原生代码）这两组对照。
- **MicroPython 的 `@native` 不是机器码**：它走 emitter，比字节码快但比手写
  原生慢，和 `rcc` 对比时要说明。
- 本基准暴露过一个真 bug：Thumb-2 的 `printf("%d")` 自 capture 提交起一直落到
  capture helper（`tinyos-script` 0206184 修复 + 验收卡）。也就是说**旧固件上
  `printf("%d")` 的输出是空的**，跑分前务必用修好的版本。
- `csc` 目前一次 `printf` 只支持一个转换，也没有位运算符 `&`；内核因此把 ms 和
  sum 分成两行打印，累加器用自减绕回而不是掩码。跨语言对照时这一点必须保持一致。

## 附加两列：clang 与 goc32

同一个 `bench/bench.c` 还可以用两个**宿主编译器**生成原生镜像，和 `rcc` 对照：

```sh
# clang：把 bench/clang/benchclang.c 放进 user/ 让正常构建（clang -Oz）编译它
mkdir -p user/benchclang
cp bench/clang/benchclang.c user/benchclang/benchclang.c
go run ./cmd/tos build -config configs/esp32c3-rir.conf
python scripts/esp-c3/flash.py --full -p COM4
# 板上：benchclang
rm -rf user/benchclang          # 这个目录只为测量存在，不要提交

# goc32：纯 Go 的 C 编译器，直接产出 LZ4TOS
(cd goc32 && go build -o bin/goc32.exe .)
goc32/bin/goc32.exe bench/bench.c        # -> bench/bench.tos
cp bench/bench.tos files.local/bin/benchgoc
go run ./cmd/tos build -config configs/esp32c3-rir.conf
python scripts/esp-c3/flash.py --full -p COM4
# 板上：benchgoc
```

`bench/clang/benchclang.c` 是 `bench/bench.c` 加上 `#include "lib.h"` 与
`TOS_STACK(4*1024)`（普通 user 程序的形状），内核与输出格式完全相同。

## 结果（2026-11-21 实测）

三块板、七个引擎，同一份 `bench/bench.c`；单位毫秒，每个数字是一次运行。
四个内核的校验和是 `46368 / 999850000 / 225792000 / 599994`，脚本逐行检查，
不一致的行不能比较。`—` = 该镜像没有这个引擎（`pvm` 需要单独的 `csa` 镜像；
MicroPython 没有 RV32 固件）。板与板之间只比同一个引擎，时钟和 ISA 都不同。

| 板 | 引擎 | fib | loop | array | div |
|---|---|---:|---:|---:|---:|
| **ESP32-C3** 160 MHz | `micropython` | 1146 | 1570 | 2411 | 876 |
| | `@native` | 748 | 631 | 1314 | 469 |
| | `pvm` | 603 | 1976 | 3518 | 1274 |
| | `rvm` | 343 | 487 | 977 | 440 |
| | `goc32` | 29 | **27** | 48 | 54 |
| | `rcc` | 84 | **27** | 71 | 58 |
| | `clang` | **15** | 17 | **26** | **27** |
| **Pico 2 M33** 150 MHz | `micropython` | 1096 | 1220 | 2015 | 696 |
| | `@native` | 635 | 530 | 1149 | 368 |
| | `pvm` | 398 | 1337 | 2358 | 841 |
| | `rvm` | 289 | 407 | 799 | 340 |
| | `goc32` | 24 | 32 | 62 | **34** |
| | `rcc` | 70 | 44 | 120 | 47 |
| | `clang` | **13** | **10** | **9** | **36** |
| **Pico 2 RV32** 150 MHz | `micropython` | — | — | — | — |
| | `@native` | — | — | — | — |
| | `pvm` | 526 | 1724 | 3078 | 1106 |
| | `rvm` | 281 | 397 | 803 | 352 |
| | `goc32` | 21 | 27 | 42 | 40 |
| | `rcc` | 61 | 27 | 60 | 42 |
| | `clang` | **14** | **15** | **14** | **20** |

\* M33 的 `goc32` `div` 原先是 1338 ms：`goc32` 的 ARM 后端对每个 `%` 都发
`bl __aeabi_idivmod`，而它用自己的前端编译 `user/aeabi.c`（预处理器不认识
`__ARM_ARCH_EXT_IDIV__`），于是落进 32 步软件循环。**#45 已修**：goc32 的 Thumb-2
后端现在在有 IDIV 的核上直接发 `udiv`/`sdiv`，余数用 `mul`+`subs` 展开；ARMv6-M
（RP2040）仍走 EABI helper。表中 34 ms 是修后重测（把 `-target rp2350` 的 benchgoc
重新生成、重建刷板），四个内核校验和全部一致。

## 读法

- **解释器组（C3）**：`rvm` 比 `pvm` 快 1.8–3.6×，比 MicroPython 字节码快
  1.9–3.3×，比 `@native` 快 1.1–1.6×。`@native` 仍是 emitter 产物，不是手写
  原生，所以它慢于 `rcc` 是预期。
- **原生组**：`clang -Oz` 通常最快；`goc32` 与 `rcc` 互有胜负（C3 的 loop
  打平；fib/array/div 上 `rcc` 慢 2–3×，因为它没有寄存器分配和指令调度）。
- **同一块 RP2350 的两个核心**：`rvm` 基本持平（289/281、407/397、799/803、
  340/352）；`rcc` 的 RV32 后端更快（fib 70→61、loop 44→27、array 120→60、
  div 47→42）；`clang` 两侧接近，RV32 的 `div` 明显更好（36→20）。
- **板与板**：C3 与 Pico 2 的时钟不同（160 vs 150 MHz），ISA 也不同
  （RV32IMC vs M33/RV32），只比同一引擎的量级，不做跨板加速比。

## 原始 `BENCH` 行（每次一行，未取中位数）

```
C3    mpy   fib 1146 loop 1570 array 2411 div  876
      @nat  fib  748 loop  631 array 1314 div  469
      pvm   fib  603 loop 1976 array 3518 div 1274
      rvm   fib  343 loop  487 array  977 div  440
      goc32 fib   29 loop   27 array   48 div   54
      rcc   fib   84 loop   27 array   71 div   58
      clang fib   15 loop   17 array   26 div   27
M33   mpy   fib 1096 loop 1220 array 2015 div  696
      @nat  fib  635 loop  530 array 1149 div  368
      rvm   fib  289 loop  407 array  799 div  340
      goc32 fib   24 loop   32 array   62 div   34   (#45 修复后)
      rcc   fib   70 loop   44 array  120 div   47
      clang fib   13 loop   10 array    9 div   36
RV32  rvm   fib  281 loop  397 array  803 div  352
      goc32 fib   21 loop   27 array   42 div   40
      rcc   fib   61 loop   27 array   60 div   42
      clang fib   14 loop   15 array   14 div   20
```

## 复现

```sh
# C3：rir 镜像（csc/rvm/rcc），bench.c 在根目录
python scripts/bench/compare.py --auto --port COM4

# Pico 2：两侧各刷一次 rir 镜像；clang/goc32 是宿主生成的
#   benchclang -> 普通用户程序；benchgoc -> goc32/bin/goc32.exe -target rp2350|rv32
python scripts/bench/compare.py --auto --port COM27 --dtr

# MicroPython（仅 M33）：刷 .tmp/micropython-pico2.uf2，然后
python scripts/bench/compare.py --mpy --mpremote "connect COM22"
```

## 没测的列与口径

- `pvm` 在两个 RP2350 镜像上都 `NOT RUN`：需要单独的 `csa` 镜像（与
  `csc/rvm/rcc` 互斥），本轮没刷。
- MicroPython 的 RV32 列不适用：RP2350 的 Hazard3 侧没有官方固件。
- 每个数字是一次运行，不是中位数；`csc` 的板上编译时间不计入（只量运行）。
- 44 程序的功能语料**不是**这个基准：`t1-benchfib` / `t1-benchloop` 的 loop
  校验和是 `-2014260032`（累加器自减绕回），与本表的 `999850000` 不同，
  不能并进上表。
