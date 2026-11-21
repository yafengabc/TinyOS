# 脚本工具链跑分：MicroPython vs pvm / rvm / rcc

同一组整数内核在同一块板上跑五个执行环境，比较**毫秒数**，不看主观感受。
本文只描述方法与脚本；结果表填在最后，RP2350 的对照以后补。

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

## 结果：ESP32-C3（2026-11-21 实测）

板子：ESP32-C3 core（CH340K），RV32IMC，160 MHz，4 MiB flash，COM4。
MicroPython：`ESP32_GENERIC_C3-20260824-v1.29.0.bin`。
TinyOS：`configs/esp32c3-rir.conf`（csc/rvm/rcc）与 `configs/esp32c3.conf`（csa/pvm）。
每次运行一遍，校验和全部一致（`46368 / 999850000 / 225792000 / 599994`）。

| 内核 | micropython | @native | pvm | rvm | goc32 | rcc | clang |
|---|---:|---:|---:|---:|---:|---:|---:|
| fib | 1146 | 748 | 603 | 343 | 29 | 84 | **15** |
| loop | 1570 | 631 | 1976 | 487 | **27** | **27** | 17 |
| array | 2411 | 1314 | 3518 | 977 | 48 | 71 | **26** |
| div | 876 | 469 | 1274 | 440 | 54 | 58 | **27** |

读法：

- **解释器组**：`rvm` 比 `pvm` 快 1.8–3.6×（fib 1.8×、loop 4.1×、array 3.6×、div 2.9×），
  比 MicroPython 字节码快 1.9–3.3×，比 `@native` 快 1.1–1.6×；
- **原生组**：`clang -Oz` 最快；`goc32` 与 `rcc` 互有胜负（loop 打平，fib/array/div
  rcc 慢 2–3×）。`rcc` 的代码生成没有寄存器分配和指令调度，这个差距是预期的；
- `@native` 相对字节码只快 1.2–2.5×：它仍是 emitter 产物，不是手写原生。

原始 `BENCH` 行（C3，一次运行）：

```
rvm   fib 343  loop 487  array 977  div 440     (ms)
rcc   fib  84  loop  27  array  71  div  58
pvm   fib 603  loop 1976 array 3518 div 1274
goc32 fib  29  loop  27  array  48  div  54
clang fib  15  loop  17  array  26  div  27
mpy   fib 1146 loop 1570 array 2411 div 876
@nat  fib 748  loop 631  array 1314 div 469
```

## RP2350（以后）

同一套脚本，把 `--port` 换成 Pico 2 的串口、`--engine rcc` 的镜像换成
`configs/rpi-pico2-*.conf`，`rcc` 用 `RCC_ARCH_THUMB2`；MicroPython 换成
`RPI_PICO2` 固件，clang/goc32 用 `--arch thumb2` 的构建。表结构不变，直接加一组行。
