# TinyOS 回归测试手册

本文件是 [`AGENTS.md`](../AGENTS.md) 的详细版。**所有 agent 在提交前都必须按这里的级别和命令执行**，并在变更说明里附上命令、目标、配置和结果。没有日志的"测试通过"不算通过。

## 0. 口径

- **能自动判定的必须自动判定**：QEMU、单元测试、构建、文件系统/窗口 soak。
- **必须人看/上板的明确标注**：PIO 的 LED/灯带、LCD 花屏、USB 枚举、真实 flash 擦写、SMP 死锁。
- **"构建通过" ≠ "功能通过"**：只说 build-only 时，不准写"验证通过"。
- **硬件不在本机就是 NOT RUN**，不是 PASS；阻塞项写入 `docs/OPEN-ITEMS.md`。
- **Pico 2 目前不是 TinyOS 支持目标**（`OPEN-ITEMS.md` §1b）。不要把 ATK-DNRP2350AM 的镜像刷到 Pico 2 上；官方 MicroPython 只用于确认 clone 的硬件/ROM/USB 正常。

## 1. 测试级别

| 级别 | 内容 | 是否阻塞提交 | 典型耗时 |
|---|---|---|---|
| **T0 静态/单元** | gofmt、`go build`、`go vet`、`go test`（含 skip 审计）、goc32/native32 测试 | **总是** | 1–3 min |
| **T1 构建矩阵** | 所有可构建目标 + 受影响的板级 target；检查产物和 `build-config-<target>.txt` | 代码/构建改动**总是** | 3–10 min |
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
for t in esp32c3 rp2040 rp2040smp rp2350 rp2350smp rp2350rv rp2350rvsmp \
         rv32 rv32smp rv32nc arm; do
  go run ./cmd/tos build -target "$t" || exit 1
done
```

PowerShell：

```powershell
$targets = "esp32c3","rp2040","rp2040smp","rp2350","rp2350smp",
           "rp2350rv","rp2350rvsmp","rv32","rv32smp","rv32nc","arm"
foreach ($t in $targets) {
  go run ./cmd/tos build -target $t
  if ($LASTEXITCODE -ne 0) { exit 1 }
}
```

- **x86 单独处理**：它在 CI/发行版工具链上仍失败（`OPEN-ITEMS.md` §5）。除非本次改动就是 x86，否则不要把 x86 失败算作回归；本机如果构建成功，记录结果。
- 每个目标检查产物：`build/<target>.uf2`、`build/esp32c3*.bin`、`build/kernel-<target>.elf`、`build/build-config-<target>.txt`。
- 记录 `build-config-<target>.txt` 的 `target`/`features`/`user-defines`，尤其是构建系统改动。
- **只读镜像也算一个目标**：只要碰了 `kernel/ramdisk.c`、`TOS_LFS_RO` 或“某段代码在
  `#ifdef` 块里外”的边界，就额外跑 `go run ./cmd/tos build -config configs/rv32-fsro.conf`
  （x86 能建就再跑一次 x86 的 RO 构建）。默认 configs 都不开 `fs_ro`，否则这条只在别人
  分支上炸（issue #14）。这个 config 随仓库走，CI 每次 push 也会构建它。

## 4. T2：QEMU

### 4.1 冒烟（每次都跑）

```sh
for t in rv32 rv32smp arm; do
  go run ./cmd/tostest smoke -target "$t" \
    -commands "ps, ls /bin | wc, piperace, prot stack, deep 3, rselftest" \
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

### 5.0.1 硬件验收分工（2026-11-17）

- **关闭 issue 的硬件门槛是 RP2350（ATK-DNRP2350AM）的 Cortex-M33 与 Hazard3 RV32
  两个 SMP 镜像**；两者都过才算过。
- **ESP32-C3 由实现侧自测**（命令见 §5.1），结果写 `docs/acceptance/`，不外包、不另开
  issue。
- **RP2040 不作为关闭依据**：目前没有自动 BOOTSEL 路径。确实需要 RP2040 证据的改动，
  单独开一个 RP2040 专项 issue，由板主另行排期；不要写进别的 issue 的关闭条件。
- **Pico 2 是独立项**（`OPEN-ITEMS.md` §1b），既不是 ATK，也不能假设同镜像能启动。

### 5.1 ESP32-C3

```sh
go run ./cmd/tos build -target esp32c3
python scripts/esp-c3/flash.py --full -p COM4
python scripts/esp-c3/accept.py -p COM4
```

- 验收是 24 项；本机 `tos.conf` 的 `[user] exclude` 若裁掉了 `hello` 等程序，会得到 23/24。**必须说明缺的是哪一个、因为配置排除**，不能把 23/24 当 24/24。
- 想跑满 24 项：用默认配置或临时配置（不要把 `tos.conf` 提交）。

### 5.2 ATK-DNRP2350AM（Cortex-M33 / Hazard3）

```sh
# 刷机 + 看启动日志（--port 用 CDC 或 UART）
python scripts/rp2350/flash.py --uf2 build/rp2350smp.uf2 --smoke -p COM17

# 验收：启动、SMP、dd+rm、信号路径
python scripts/rp2350/accept33.py --image rp2350smp --no-flash -p COM17 --dd-rounds 1000
python scripts/rp2350/accept33.py --image rp2350rvsmp --no-flash -p COM17 --dd-rounds 100

# PIO（自动部分 + 需要人看的 LED/灯带用 --visual --strip）
python scripts/rp2350/pio_accept.py --image rp2350smp --no-flash -p COM17
python scripts/rp2350/pio_accept.py --image rp2350smp --no-flash -p COM17 --visual --strip

# SMP 专项（hang/rate/topology），可选 --func 带 shell 检查
python scripts/rp2350/smptest.py -p COM17 --func

# kernel threads（只有该 feature 打开时）
python scripts/rp2350/kthread_accept.py --uf2 build/rp2350smp.uf2 --out build/kthread.log
```

- 启动日志至少要看到 `[A]/[B]/[C]/[D]/[E]`、`2 of 2 CPU(s) online`、USB CDC `2e8a:000a`。
- 信号路径：`sh` 里 `sleep 3000` 后发 `^C`，控制台必须回到 `msh`，无 HardFault/死锁。

### 5.3 RP2040

> **不是关闭依据**（2026-11-17）：无自动 BOOTSEL 路径。需要 RP2040 证据时单开专项
> issue，由板主排期。下面的命令仍保留给专项验收使用。

```sh
python scripts/rp2040/autoflash.py --uf2 build/rp2040.uf2
python scripts/rp2040/heartbeat.py --port COM19
python scripts/rp2350/pio_accept.py --image rp2040smp --uf2 build/rp2040smp.uf2
```

- RP2040 的窗口数字与守卫已在 2026-11-17 人工核对（`HISTORY.md` §43）：`wink=16`、`deep 3` 增长、`deep 6` kill；`deep 6 0 8` 也已验证会触发（`deep.c` 现在 sleep(0) 也进入 syscall）。
- `scripts/rp2040/heartbeat.py --port COM19` 已在最终单核镜像上 **6/6**（启动、时间、fs 读、用户程序、fs 写回、crash 隔离）。
- PIO 的 LED/灯带是目视项，必须写"看了/没看"，不能只写"PIO 通过"。

### 5.4 Pico 2（暂不支持）

- TinyOS 的 `board/rp2350` 是 ATK-DNRP2350AM 板级目标（LED GPIO3、LCD/TF、4 MiB 文件系统假设），**不是 Pico 2 target**。
- 现状：官方 MicroPython 在 clone 上正常；TinyOS `rp2350smp.uf2` 早期 HardFault。已记在 `OPEN-ITEMS.md` §1b。
- 规则：不要把 ATK 镜像刷到 Pico 2；Pico 2 适配要单独立项（独立 board target、正确的 LED/无 LCD/实际 flash 大小），并为它建立 T3 验收后再纳入回归矩阵。

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
- 7 个镜像构建（esp32c3、rp2040/smp、rp2350/smp、rp2350rv/rvsmp）；
- `configs/rv32-fsro.conf` 的只读镜像构建（issue #14 的盲区）；
- rv32/rv32smp/arm 的 QEMU smoke（含 `rselftest`）+ `scripts/soak.py` 3 轮；
- tag 触发的 Release（`release` 依赖 `[checks, build, smoke]`）。

**不覆盖**：

- x86 构建与链接（`OPEN-ITEMS.md` §5：发行版 lld 拒绝 i386 PIE 的 16 位重定位）；
- `rv32nc`：整个目标不在 CI 里构建（T1 本地跑）；
- 任何真机、Pico 2、PIO 目视项；
- 长时间 SMP/flash soak（CI 只有 3 轮）。

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
