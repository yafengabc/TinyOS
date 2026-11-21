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

## 结果（待填）

板子：ESP32-C3，160 MHz，`configs/esp32c3*.conf`；每次运行取 1 次（抖动大的再加）。

| 内核 | micropython | @native | pvm | rvm | rcc |
|---|---:|---:|---:|---:|---:|
| fib | | | | | |
| loop | | | | | |
| array | | | | | |
| div | | | | | |

记录：镜像 commit、MicroPython 版本、串口、原始 `BENCH` 行。

## RP2350（以后）

同一套脚本，把 `--port` 换成 Pico 2 的串口、`--engine rcc` 的镜像换成
`configs/rpi-pico2-*.conf`，`rcc` 用 `RCC_ARCH_THUMB2`；MicroPython 换成
`RPI_PICO2` 固件。表结构不变，直接加一列。
