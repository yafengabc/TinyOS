# TinyOS 回归测试手册

本文件是 [`AGENTS.md`](../AGENTS.md) 的详细版。**所有 agent 在提交前都必须按这里的级别和命令执行**，并在变更说明里附上命令、目标、配置和结果。没有日志的"测试通过"不算通过。

## 0. 口径

- **能自动判定的必须自动判定**：QEMU、单元测试、构建、文件系统/窗口 soak。
- **必须人看/上板的明确标注**：PIO 的 LED/灯带、LCD 花屏、USB 枚举、真实 flash 擦写、SMP 死锁。
- **"构建通过" ≠ "功能通过"**：只说 build-only 时，不准写"验证通过"。
- **硬件不在本机就是 NOT RUN**，不是 PASS；阻塞项写入 `docs/OPEN-ITEMS.md`。
- **Pico 2 从 2026-11-18 起是可启动的板级目标**（`rpi-pico2`，见 `OPEN-ITEMS.md` §1b 与
  `TESTING.md` §5.4）。仍然不要把 ATK-DNRP2350AM 的镜像刷到 Pico 2 上；官方 MicroPython
  只用于确认 clone 的硬件/ROM/USB 正常。

## 1. 测试级别

| 级别 | 内容 | 是否阻塞提交 | 典型耗时 |
|---|---|---|---|
| **T0 静态/单元** | gofmt、`go build`、`go vet`、`go test`（含 skip 审计）、goc32/native32 测试 | **总是** | 1–3 min |
| **T1 构建矩阵** | 所有可构建目标 + 受影响的板级 target；检查产物和 `build/build-config.txt` | 代码/构建改动**总是** | 3–10 min |
| **T2 QEMU** | rv32 / rv32smp / arm 的 smoke + soak；必要时 compat | 内核/arch/驱动/用户态改动**总是** | 3–8 min |
| **T3 真机** | 两块门槛：**心跳级**（新板最低线，§5.0）与**平权级**（C3、ATK RP2350 ARM/RV32、RP2040 的验收/目视项） | 板级/驱动/控制台/SMP 改动**按需但必须明确** | 5 min（心跳）/ 10–40 min（平权） |
| **T4 压力** | 1000 轮 `dd`+`rm`、SMP soak、长时间 flash/窗口抖动 | 锁/内存/flash/窗口改动 | 10–60 min |

## 2. T0：静态与单元

```sh
# 1) 格式（输出必须为空；只看本仓库跟踪的 .go）
git ls-files '*.go' | xargs gofmt -l

# PowerShell:
# git ls-files '*.go' | ForEach-Object { gofmt -l $_ }

# 2) 编译与静态检查
go build ./...
go vet ./...

# 3) 先构建一个目标，减少依赖产物的测试被 skip
go run ./cmd/tos build -target rv32

# 4) 单元测试（禁止用缓存结果）
go test ./... -count=1

# 5) skip 审计：记录本轮的 skip 列表，和基线比较；不允许新增
go test ./... -count=1 -v > build/testlogs/go-test.log
grep -- '--- SKIP' build/testlogs/go-test.log || true
# PowerShell:
# Select-String -Path build\testlogs\go-test.log -Pattern '--- SKIP'

# 6) goc32 单文件循环（动过 user/lib.h、include/*.h 或 ABI 结构时必跑）
cd goc32
go run . ../user/hello.c -o ../build/goc32-hello.tos
cd ..
```

**为什么第 6 步在 T0 里**：goc32 不是 clang，它自己解析共享头。`tos_abi.h` 里的
`offsetof` 断言（`__builtin_offsetof`）就曾让**所有**用户程序在 goc32 下编译失败，
而 clang 构建、QEMU、真机全部通过——只有这一条能发现。

**规则**

- `t.Skip` 只能用于"缺少外部产物/工具链"；必须在测试里写明原因，并在 PR 里列出。**不允许为了绿而新增 skip**。
- 依赖 `build/` 产物的测试，先在 T0 第 3 步生成对应目标；如果仍 skip，记录为 `NOT RUN`。
- 改了 goc32 子模块时另跑：

```sh
cd goc32
go test ./... -count=1
cd internal/native32
go test ./... -count=1
```

## 3. T1：构建矩阵

至少覆盖 CI 的 7 个目标；改构建系统/`Targets`/`config.go` 时跑全部可构建目标：

```sh
go run ./cmd/tos build all          # 15 个目标逐个构建；失败会汇总后非零退出
```

`build all` 不应用 `-config`/`tos.conf`：镜像配置是逐目标的，要带 config 就单独
`-target` 构建。等价的老写法是逐个 `-target` 循环，现在只是把它收进工具里。

- 每个目标检查产物：`build/tinyos-<target>.uf2`、`build/tinyos-esp32c3*.bin`、
  `build/kernel-<target>.elf`、`build/build-config.txt`。
- 记录 `build/build-config.txt` 的 `target`/`features`/`user-defines`，尤其是构建系统改动。
- **只读镜像也算一个目标**：只要碰了 `kernel/ramdisk.c`、`TOS_LFS_RO` 或“某段代码在
  `#ifdef` 块里外”的边界，就额外跑 `go run ./cmd/tos build -config configs/rv32-fsro.conf`
  （x86 能建就再跑一次 x86 的 RO 构建）。默认 configs 都不开 `fs_ro`，否则这条只在别人
  分支上炸（issue #14）。这个 config 随仓库走，CI 每次 push 也会构建它。
- **发布/静默镜像也算一个目标**：碰了 `TOS_BOOT_TRACE`/`TOS_DIAG` 或启动打印时，至少用
  `go run ./cmd/tos build -target <board> -config configs/release.conf` 构建一次，确认
  静默镜像仍然链接通过（`[build] boot_trace` 是唯一开关，CI 发布矩阵就用它）。

## 4. T2：QEMU

### 4.1 冒烟（每次都跑）

```sh
for t in rv32 rv32smp arm; do
  go run ./cmd/tostest smoke -target "$t" \
    -commands "ps, ls /bin | wc, cat /proc/execfmt, piperace, prot stack, deep 3, rselftest" \
    -commandwait-ms 4000 || exit 1
done
```

预期：每个目标最后输出 `SMOKE TEST OK`；`piperace` 有 `piperace: ok`；`prot stack` 打印保护错误但内核继续；`deep 3` 完成。

### 4.2 Soak（内核/内存/窗口/锁改动必跑）

```sh
python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm
```

### 4.3 跨目标兼容（可选，但改输出/ABI/文件系统时建议）

```sh
go run ./cmd/tostest compat -targets rv32,arm
```

x86 暂不在默认列表；原因见 `OPEN-ITEMS.md` §5。

### 4.4 RV32 原生镜像（改 `rcc` / RIR2 / ELF 加载时必跑）

宿主 `tools/test.py` 既跑 `csc -> rvm`，也生成 `rcc` 的 `.tos`；ELF 必须在目标
内核的加载器里跑。`rvm` 的指令区已改成按页读取，`bigcode`（535 IR / 10 KiB text）
这类程序在宿主也能被解释器校验，不再有 native-only 豁免。流程分两个仓库：

```sh
# 1. tinyos-script：生成 RIR2 和 build/<name>.tos（40 个程序用例）
python tools/test.py

# 2. TinyOS：把要跑的 .tos 放进 files.local/bin/。files/ 是受版本控制的静态
#    文件，生成物只写 files.local/（.gitignore 已忽略）；同名文件覆盖 files/。
mkdir -p files.local/bin
cp ../tinyos-script/build/bigcode.tos files.local/bin/rt-bigcode

# 3. 构建 rv32 rir 镜像（含 csc+rvm+rcc，关掉 csa+pvm）
go run ./cmd/tos build -config configs/rv32-rir.conf

# 4. 冒烟：程序输出要和 rvm 一致（rvm 的 result= 汇总行原生镜像不打印）
go run ./cmd/tostest smoke -target rv32 -commands "rt-bigcode" -commandwait-ms 25000
```

注意：

- qemu-virt 的 ramdisk 是 512 KiB（`ramdisk_sectors = 1024`），40 个 `.tos` 约
  200 KiB 装不下；一次 10–15 个，分批重建并冒烟。
- 原生镜像只打印程序自己的输出；`rvm` 末尾的 `result=... ir=...` 不会出现，
  比对时先去掉这一行。
- 板上 `csc`：函数表 256（`FUNC_MAX`）、单函数 256 个 AST 节点（`NODE_MAX`）、
  256 条 IR。`many25`/`many32`/`bigcode`/`bigir` 现在都能在板上编译；
  90 条语句的 `bigbody`（约 450 节点）仍需宿主 `csc`。窗口 128 KiB。
  见 `OPEN-ITEMS.md` §12。
- 板上 `rcc`：RIR2 ≤ 40 KiB（约 2000 IR，按 4 KiB 分块读）、4096 个标签、
  text ≤ 48 KiB；C3/rv32 的窗口是 88 KiB。`tests/bigir.c`（1436 IR / 2062 标签）是这套
  上限的回归用例，`tests/hugeir.c`（2948 IR）只在宿主跑。
- `bloop3` / `sieve3` / `deep 3` 在 TCG 下超过 10 s，`-commandwait-ms` 要给够。

### 4.5 ARM Thumb-2 后端（实验，改 `thumb2.inc` 时）

`rcc` 的 ARM 后端已完成 stage 2：整数/比较/跳转/调用、全局/数组、字符串
（strlen/strcmp/strlcpy/strlcat/atoi/print）、Q16.16 float（fmul/fdiv 与
`print`/`printf` 格式化）、GPIO/ADC/PWM/sleep/uptime 的 native 系统调用。
流程与 §4.4 相同，只是目标换成 mps2：

```sh
# 宿主生成 RIR2 与 .tos（tools/test.py 已产出 build/<name>.tos 的是 RV32）
# ARM 镜像用 csc 现场编译，或把宿主的 .rir 传上去：
go run ./cmd/tos build -config configs/arm-rir.conf
go run ./cmd/tostest smoke -target arm \
  -commands "csc /fold.c /f.rir,rcc /f.rir /bin/nf,nf" -commandwait-ms 15000
```

已验证 28 个用例，输出与 `rvm` 逐行一致：`fold mod flow flow2 switch
switch2 arith cmp2 loops2 funcs spill deep many25 many32 bigswitch bigbody
arrays2 copy nest sret native many20 bloop3 sieve3 fib24 struct3 ops
strings2 str globals2 mixed float3 fmt sa3 floatfold`，以及
`bigcode 12829 / bigir 1325 / many100 20009`（这三个在 2026-11-21 之前是
native-only，`rvm` 分页后进入普通对照）。

## 5. T3：真机

端口、板子、镜像必须写进证据；下面是模板，`COMx` 换成实际端口。

### 5.0 两级门槛：心跳级 / 平权级

新板接入先过**心跳级**，成熟后才要求**平权级**。两级都不接受"看起来能跑"。

**心跳级**（新板最低线，5 分钟内、非交互）：

| # | 项 | 判据 |
|---|---|---|
| 1 | 启动 | banner + `tos> ` prompt |
| 2 | 时间 | 两次 `uptime` 之间宿主 `sleep 1`，差值 1±0.1 s（只测"递增"会让 tick 错 10 倍也通过） |
| 3 | 文件系统读 | `ls /bin` 有输出 |
| 4 | 用户程序 | `hello`（或任一 `exec` 成功的程序） |
| 5 | 文件系统写回 | `wfile hb.tmp x` → `cat` 读到 → `rm`（`exec` 只覆盖读路径，prog 是新板最容易坏的一条） |
| 6 | fault 隔离 | `crash` 之后内核仍回 prompt——这是"内核没死"的唯一证据 |

- 脚本固定为 `scripts/<board>/heartbeat.py --port <p> --target <t>`，末行
  `HEARTBEAT OK n/n`、退出码 0/1；**脚本随 target 一起入库**，否则新板永远
  "没脚本 → 不进 CI → 漂移"。
- 心跳级通过不等于板子可用；它挡的是"改内核把某板搞死却没人发现"。
- `prot` 四项、`reboot`、屏幕、PIO **不在**心跳级；它们属于平权级。

**平权级**（成熟板，现有 `accept.py` 全项）：

- 通过标准是 **24/24，或具名 N/24**。禁止"按板能力取适用子集"这种模糊措辞，
  禁止把 N/24 记成 24/24。
- 不入项分两种，都要在 `OPEN-ITEMS.md` 有具名条目：
  1. **构建期不存在**（例：RP2040 无 MPU/PMP，`prot` 不编译）→ 报告 `n/m`，
     `m` 是本镜像实际构建的项；
  2. **构建了但运行失败/降级**（例：P4 PMP 粒度不足）→ 具名降级，写清硬件原因。
- 只有第 2 种叫"降级"；两种都不许写成 24/24。

### 5.0.1 硬件验收分工（2026-11-19）

- **关闭 issue 的 RP2350 硬件门槛是 Pico 2 的两个 SMP 镜像**
  （`rpi-pico2-smp` Cortex-M33、`rpi-pico2-rv32-smp` Hazard3）：心跳 6/6、
  `dd_stress.py` ≥100 轮、`smpbench` ≥1.8x，三者都过才算过。
- **为什么换成 Pico 2**：8 MiB flash 让高强度 dd/soak 跑得开，板子也更便宜；
  ATK-DNRP2350AM 继续支持、镜像继续构建，但不再是关闭门槛。
- **ESP32-C3 由实现侧自测**（命令见 §5.1），结果写 `docs/acceptance/`，不外包、不另开
  issue。
- **RP2040 不作为关闭依据**：目前没有自动 BOOTSEL 路径。确实需要 RP2040 证据的改动，
  单独开一个 RP2040 专项 issue，由板主另行排期；不要写进别的 issue 的关闭条件。
- 镜像是板级的：ATK 的 24 项验收表不等于 Pico 2 的证据，反之亦然。

### 5.1 ESP32-C3

```sh
go run ./cmd/tos build -target esp32c3-generic
python scripts/esp-c3/flash.py --full -p COM4
python scripts/esp-c3/accept.py -p COM4
```

- 验收是 24 项；本机 `tos.conf` 的 `[user] exclude` 若裁掉了 `hello` 等程序，会得到 23/24。**必须说明缺的是哪一个、因为配置排除**，不能把 23/24 当 24/24。
- 想跑满 24 项：用默认配置或临时配置（不要把 `tos.conf` 提交）。

### 5.2 ATK-DNRP2350AM（参考板，Cortex-M33 / Hazard3）

> 自 2026-11-19 起 ATK **不是关闭门槛**（门槛见 §5.0.1 与 §5.4）。下面的命令保留
> 用于回归、对照和 24 项表。

```sh
# 刷机 + 看启动日志（--port 用 CDC 或 UART）
python scripts/rp2350/flash.py --uf2 build/tinyos-atk-dnr2350am-smp.uf2 --smoke -p COM17

# 验收：启动、SMP、dd+rm、信号路径
python scripts/rp2350/accept33.py --image atk-dnr2350am-smp --no-flash -p COM17 --dd-rounds 1000
python scripts/rp2350/accept33.py --image atk-dnr2350am-rv32-smp --no-flash -p COM17 --dd-rounds 100

# PIO（自动部分 + 需要人看的 LED/灯带用 --visual --strip）
python scripts/rp2350/pio_accept.py --image atk-dnr2350am-smp --no-flash -p COM17
python scripts/rp2350/pio_accept.py --image atk-dnr2350am-smp --no-flash -p COM17 --visual --strip

# SMP 专项（hang/rate/topology），可选 --func 带 shell 检查
python scripts/rp2350/smptest.py -p COM17 --func

# kernel threads（只有该 feature 打开时）
python scripts/rp2350/kthread_accept.py --uf2 build/tinyos-atk-dnr2350am-smp.uf2 --out build/kthread.log
```

- 启动日志至少要看到 `[A]/[B]/[C]/[D]/[E]`、`2 of 2 CPU(s) online`、USB CDC `2e8a:000a`。
- 信号路径：`sh` 里 `sleep 3000` 后发 `^C`，控制台必须回到 `msh`，无 HardFault/死锁。

### 5.3 RP2040

> **不是关闭依据**（2026-11-17）：无自动 BOOTSEL 路径。需要 RP2040 证据时单开专项
> issue，由板主排期。下面的命令仍保留给专项验收使用。

```sh
python scripts/rp2040/autoflash.py --uf2 build/tinyos-rpi-pico.uf2
python scripts/rp2040/heartbeat.py --port COM19
python scripts/rp2350/pio_accept.py --image rpi-pico-smp --uf2 build/tinyos-rpi-pico-smp.uf2
```

- RP2040 的窗口数字与守卫已在 2026-11-17 人工核对（`HISTORY.md` §43）：`wink=16`、`deep 3` 增长、`deep 6` kill；`deep 6 0 8` 也已验证会触发（`deep.c` 现在 sleep(0) 也进入 syscall）。
- `scripts/rp2040/heartbeat.py --port COM19` 已在最终单核镜像上 **6/6**（启动、时间、fs 读、用户程序、fs 写回、crash 隔离）。
- PIO 的 LED/灯带是目视项，必须写"看了/没看"，不能只写"PIO 通过"。

### 5.4 Pico 2（RP2350 验收门槛）

板级目标是 `board/rpi-pico2`（`base = rp2350-generic`）：LED 在 GP25（高电平点亮）、
无 LCD/TF、8 MiB W25Q64。四个镜像：`rpi-pico2`、`rpi-pico2-smp`、
`rpi-pico2-rv32`、`rpi-pico2-rv32-smp`。

**关闭门槛用两个 SMP 镜像**：

```sh
# 心跳 6/6（启动、时间、fs 读、用户程序、fs 写回、fault 隔离）
python scripts/rp2350/flash_heartbeat.py --uf2 build/tinyos-rpi-pico2-smp.uf2 \
    -p COM19 --target rpi-pico2-smp
python scripts/rp2350/flash_heartbeat.py --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2 \
    -p COM19 --target rpi-pico2-rv32-smp

# dd/rm 至少 100 轮（8 MiB flash 的意义就在这里）
python scripts/rp2350/dd_stress.py -p COM19 --target rpi-pico2-smp --rounds 100
python scripts/rp2350/dd_stress.py -p COM19 --target rpi-pico2-rv32-smp --rounds 100

# 双核扩展性：在 shell 里 smpbench 1250000，期望 >= 1.8x

# 重启稳定性（每次都要 < 2 s，且新 shell 自己的 uptime 必须归零）
python scripts/rp2350/reboot_soak.py --uart COM19 -n 20
```

单核镜像和 `accept33.py` 的 24 项表仍可用于排查，但**不是关闭依据**（那张表是 ATK
的 LCD/TF/按键假设）。

- UART0 在 GP0/GP1（115200 8N1，需要一个 USB-TTL 适配器）；板上 USB（2e8a:000a）
  是另一个 console，`-p COM17 --dtr` 走它。
- **刷完按一次 RUN**：这块克隆板的 ROM 在 UF2 写完不保证自动重启应用。刷入后
  3–5 s 内没有 `[A]`/`tos>` 就按一次 RUN；`flash.py --smoke` 遇到这种情况会一直等，
  可以 Ctrl-C 后按 RUN 再 `--no-flash --smoke` 重看启动日志。
- 启动日志应看到 `[A] clocks+reset ok  clk_sys=00b71ee8` → `[B] clk=0x08f0d180`
  → `[C]` → `[D]` → `[E] ... clk_usb_khz=0000bb80` → `tos>`。
- **2026-11-18 前的失败已修**（`cff442f`）：不是 XIP 时序，而是三处叠加 ——
  clk_sys 切换在 flash 里执行、`boot2-w25q080` 在 ROM 已配好四线之后又改 QMI/SR2、
  以及首行串口用猜测的 12 MHz 算分频（把前面两处伪装成“死机”）。
  详见 `docs/PICO2-VS-ATK-BOOT.md` 与 `docs/acceptance/2026-11-18-pico2-boot-rp2350-cff442f.md`。
- 仍不要把 ATK 镜像刷到 Pico 2（LED/屏幕/flash 大小都不同），也不要反过来。
- 未纳入的项：屏幕、TF 卡、PIO 灯带（板子没有这些外设）；`accept33.py` 的 24 项表
  只适用于 ATK 四个镜像。
- GP25 上的 1 Hz 心跳目前**不会亮**（`led_set()` 仍硬编码 GP3，issue #27）；
  判断“内核活着”请看串口或 `uptime`。
- **重启要单独测**：`reboot_soak.py` 的两个判据缺一不可——提示符要在 2 s 内出现，
  并且之后 `uptime` 必须很小。只看“提示符来了”分不出“真的复位了”和“根本没复位、
  看到的是上一个 shell 的提示符”，第一版 `gate_boot.py` 就是这样假通过 15/15 的。
  2026-11-20 的 33 s 静默（`HISTORY` §85）就是被这个判据抓出来的。

## 6. T4：压力

```sh
# 真机窗口/段/堆抖动（ATK）
python scripts/rp2350/soak_board.py COM17 100 build/soak-board.log

# C3 flash/窗口
python scripts/esp-c3/soak.py -p COM4 --cycles 100

# QEMU 长时间
python scripts/soak.py --cycles 8 --targets rv32,rv32smp,arm
```

判定：`free` 前后的 kernel heap/free 和 tasks 数字只允许峰值水位变化；出现一次窗口未归还、段陈旧或 `free` 漂移就是失败。

## 7. 改动类型 → 必跑级别

| 改动 | T0 | T1 | T2 | T3 | T4 |
|---|---|---|---|---|---|
| 纯文档 | ✅ | — | — | — | — |
| 构建系统 / `Targets` / `config.go` | ✅ | 全目标 | smoke | 受影响板 | — |
| 内核调度/进程/锁/tty/console | ✅ | ✅ | smoke+soak | ATK SMP + C3（共享路径） | dd/SMP soak |
| arch / board / port | ✅ | ✅ | 该架构 | 该板 | 视改动 |
| 驱动 | ✅ | ✅ | 可模拟的部分 | 带该设备的板 | — |
| 用户态/libc/shell | ✅ | ✅ | smoke | 控制台相关板 | — |
| LZ4TOS / ELF / ABI / 协议 | ✅ | ✅ | 全量 | 兼容向量 + 板 | — |
| goc32 / native32 | ✅（含两个子模块测试） | 相关目标 | 单文件 dev loop | 相关板（可选） | — |

## 8. 大重构专用协议

1. **冻结基线**：记录 `git rev-parse HEAD`、`build-config.txt`、T0/T1/T2 日志；有硬件就加 T3 基线。  
2. **先写特征测试**：把要动的行为固定下来（shell 输出、`ps` 形状、窗口 span、`free`、LZ4TOS 头、错误路径）。不要先改代码再补测试。  
3. **小步提交**：每个提交自己跑完对应级别；不允许"最后一次全跑"。  
4. **对比输出要归一化**：PID、uptime、tick、地址、耗时数字先屏蔽；不要把 `ps` 的 PID 差异当回归。  
5. **行为变更单独提交**：重构提交里不允许改预期输出；需要改测试时，单独提交并写清"这是有意的行为变更 + 理由"。  
6. **硬件签字**：T3 结果要带板子、端口、镜像、命令；没有硬件就写 `NOT RUN` 并把阻塞记入 `OPEN-ITEMS.md`。  
7. **收口**：最终提交附证据表（见 §10），并确认 skip 列表没有增长。

## 9. 失败处理

- **先复现 3 次**，保存完整日志；区分稳定失败/偶发。  
- **对照基线**：用 `git worktree add ../tos-baseline <sha>` 在基线跑同一条命令。基线也失败 → 预存在问题，记 `OPEN-ITEMS.md`；只有基线通过才算回归。  
- **环境问题**（没板子、没 QEMU、没 clang/zig）→ `NOT RUN`，不准写成 `PASS`。  
- **禁止**用 `t.Skip`、删断言、放宽阈值、盲改 golden 来"修复"。  
- 修复后必须重跑同一级别，并在说明里写"失败 → 修复 → 通过"的命令和结果。

## 10. 证据模板

复制到 `docs/acceptance/` 的一份摘要（一次运行一个文件，命名与索引见
[`acceptance/README.md`](acceptance/README.md)），并在 PR/提交说明里链接它。
**原始日志不入库**：留在本地或 CI artifact；确需长期保存的贴到 issue/commit。

```text
commit: <短 sha>（被测提交）
change: <一句话>
baseline: <sha>（重构/修复前；没有就写 none）
target/config: <target> / <config file 或 defaults>
board/port/image: <板子/端口/镜像；纯 QEMU 写 target>
toolchain: go <ver>, clang <ver>, qemu <ver>, python <ver>
host:
T0: go build/vet/test ... -> PASS (skips: N, list)
T1: targets ... -> PASS/FAIL
T2: smoke rv32/rv32smp/arm -> PASS; soak -> PASS
T3: board/port/image -> PASS/NOT RUN (last UART line)
T4: rounds -> PASS
logs: <本地路径；或 "not kept">
notes:
```

## 11. CI 覆盖与盲区

当前 `.github/workflows/images.yml` 覆盖：

- `checks` job：`go test ./...`、`go vet ./...`（每个 push）；
- 11 个发布镜像构建（esp32c3-generic、rpi-pico/smp、atk-dnr2350am/smp、
  atk-dnr2350am-rv32/smp、rpi-pico2/smp、rpi-pico2-rv32/smp），
  统一加 `-config configs/release.conf`（`boot_trace = 0`）：发布件不打印 `[A]..[E]`、
  `[P1]/[P2]`、`[smp]`、`[mpu]` 等启动轨迹；
- `smoke` 是**按目标并行**的一组 job（每个约 50–95 s）：
  - `configs/rv32-fsro.conf` 的只读镜像构建（只编译，不起机；issue #14 的盲区）；
  - rv32 / rv32smp / arm / x86 各自构建 + 起机 + `ps`、`ls`、`piperace`、`prot stack`、
    `deep 3`、`rselftest`；
- tag 触发的 Release（`release` 依赖 `[checks, build, smoke]`）。

**不覆盖**：

- **x86 的发布资产**：它能在发行版工具链上构建并起机（HISTORY §60），但没有原始磁盘
  镜像的打包路径，所以只在 smoke 里跑，不进 Release（`OPEN-ITEMS.md` 已无此条）；
- `rv32nc`：整个目标不在 CI 里构建（T1 本地跑）；
- **`scripts/soak.py`**：2026-11-17 起从 CI 移出（它只是"冒烟的冒烟"）。soak 属于本地
  T2/T4，要带日志；
- 任何真机、Pico 2、PIO 目视项。

所以 **CI 绿 ≠ 回归通过**；按本手册的 T0–T4 执行并在说明里注明哪些是本地/硬件结果。

## 12. 提交前检查表

- [ ] T0 通过；skip 数没有增加，新增 skip 有原因。
- [ ] T1 覆盖受影响目标和 CI 7 目标；x86 失败按 §3 记录。
- [ ] T2 smoke + soak 通过（内核/arch/驱动/用户态改动）。
- [ ] T3 按 §5 执行或明确 `NOT RUN`，并记录板子/端口/镜像。
- [ ] T4 按改动类型执行（锁/内存/flash/窗口）。
- [ ] 日志、命令、配置在说明里；没有"应该没问题"。
- [ ] 没有为绿改测试/删断言/盲改 golden。
- [ ] 预存在失败/环境阻塞已写入 `docs/OPEN-ITEMS.md`。
