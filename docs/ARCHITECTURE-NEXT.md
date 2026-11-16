# TinyOS 架构演进提案：稳定 v1（接缝先行）

- **状态**：提案，未实施。本文只定结构与契约，不实现具体功能。
- **基线**：当前 `main`（代码 `fea309f`，2026-11-16；本文档提交在其后）。行号会漂移，定位以函数名/符号名为准。
- **本版目标**：**一次性把稳定架构的"接缝"和"兼容承诺"定对**，实现允许分批；不追求一个大 PR 把所有板子/驱动/格式一起改完。
- **与 review 的边界**：`PROJECT-REVIEW-2026-11-16.md` 的具体小修继续由执行流处理；本文只处理跨子系统、会改变目录或接口契约的部分。

---

## 0. 什么是"稳定"

稳定不等于"代码不再变"，而是：

1. **契约稳定**：板级能力、ABI/格式、跨核独占、内存域、驱动接缝、等待语义各有唯一入口和版本号；
2. **实现可替换**：接缝后面可以换实现（RP → P4、本地 → companion），接缝本身不变；
3. **既有目标语义不变**：12 个目标的行为、shell、窗口、SMP 性能在 Phase A 前后一致。

"一次性"指的是**一次把接缝和兼容承诺定对**，不是一次把所有实现写完。

### 0.1 必须冻结的 6 个契约

| # | 契约 | 冻结什么 | 现在给什么实现 | 以后怎么扩展 |
|---|---|---|---|---|
| C1 | **board/chip/config** | `board.conf`（INI，`schema=1`）+ `board/<chip>` / `board/<vendor>/<board>` 分层 + 能力校验 | 只声明能力 + 构建期校验；不生成头、不搬 `layout.c` | 生成 `build/gen/board_config.h`；加 variant（Pico 2） |
| C2 | **ABI / 格式** | `include/tos_abi.h`、`abi/syscalls.csv`、`exec_format`、LZ4TOS flags 版本位 | 结构/编号单一头 + 静态断言；LZ4TOS/ELF 各一个 handler | 新 syscall 追加；新格式注册 handler；flags 高字节版本 |
| C3 | **flash/跨核独占** | `plat_flash_exclusive_begin/end` + 协作式 park poll + `plat_cache_*`/`plat_dma_*` 钩子 | RP 现有 park 包一层，行为不变；cache/DMA 钩子 no-op | 三块 RP 共享骨架；P4 实现自己的独占；不承诺通用 pause |
| C4 | **内存域/窗口** | `struct mem_domain` 列表（现在 1 个）+ `win_alloc` 带域参数 | 单域，行为与现在完全一致 | P4 PSRAM 需要时加第二域；不提前做多域 |
| C5 | **驱动接缝** | `dev_ops` + LCD `lcd_lowops` | LCD 的 RP lower 接到 ops；输出不变 | P4/新板补 lower；不强制 GPIO/SPI/WiFi 全量迁移 |
| C6 | **等待/超时** | `wd_start(cb, ticks)` 的小接口 | 先接 1–2 个现有等待点 | 网络/USB/companion 复用；所有阻塞 I/O 有上限 |

### 0.2 兼容承诺（违反一条就必须单独评审，不能混进架构提交）

- 既有 syscall 编号**只追加，不改语义**；
- 既有结构**只追加字段，不重排**；`_Static_assert(sizeof(...))` 守住；
- 旧 V1 `.tos` 在新内核继续加载；
- 12 个既有目标的启动/shell/窗口/SMP 行为不变；
- `board.conf schema`、`tos_abi`、LZ4TOS flags、`exec_format` 都带版本号；扩展是加字段/加 handler，不是重写；
- 不把 Pico 2 的未解决问题带进回归基线（`OPEN-ITEMS.md` §1b）。

---

## 1. 现状再评估

| 条目 | 上一版 | 现在 | 影响 |
|---|---|---|---|
| 窗口几何 | `WindowGeometry()` 16K/8K/4K 漂移 | **已修**：4K/4K/4K + `geometry_test.go`（`59a09e5`） | C1 只剩配置冲突与单一真相 |
| N-01/N-02/N-03 | SysTick/park/wdt | **已修**（`19352a4`） | C3 保留，动机转为防分叉 |
| N-04/N-05 | 信号/唤醒锁外 | **已修**（`d577ab8`、`fa5b276`） | 单独回归 |
| N-06 / file_t | LCD DMA 竞态、fd 体积 | **已修**（`a2c3fe1`） | C5 的 LCD 试点仍做 upper/lower，竞态已单独修完 |
| B-6 bounce buffer | 所有读经 4 KiB bounce | **已修**（`fea309f`） | 存储路径的性能基线已变，回归时注意 |
| CI Go 检查 | 无 `go test`/`go vet` | **已加**（`82a3c53`） | C1/C2 的验收可进 CI |
| goc32 | 未初始化 | **已初始化/自包含**（子模块 `7a17826`），feature 宏从 `build-config-<target>.txt` 导入 | C2 的黄金向量可做；生成表需保留快照 |
| Pico 2 | 未记录 | **`OPEN-ITEMS.md` §1b**：`rp2350smp` 早期 HardFault（`PC=0xeffffffe`），MicroPython 正常 | C1 必须支持同芯片 variant；`board/rp2350` 不能假设 ATK 的 LED/LCD/4MiB |
| `-target`/conf 冲突 | 静默套用 features | **仍存在**（`ApplyConfig()` 只在命令行未选 target 时读 conf target） | **C1 第一个要做的** |
| flash park | 三块板各一套 | 仍如此（超时已修，协议未收敛） | C3 |
| ABI 布局 | `stat/meminfo/...` 只在 `user/lib.h` | 仍如此 | C2 |
| 内存域 | 单一连续窗口区 | 仍如此 | C4，保持条件性 |

---

## 2. P4 分水岭

官方规格（Espressif 产品页，2026-11-16）：**双 HP RISC-V（400 MHz）+ LP 核（40 MHz）**，768 KB 片上 L2MEM，8 KB TCM，最多 32 MB PSRAM，无片内无线（ESP32-C6 companion），MIPI-CSI/DSI、USB 2.0 HS。

用户已明确：**LP 按协处理器处理，不进 SMP**。

P4 挑战五个假设：核同构、内存单域、外设本地 MMIO、启动链是 PICOBIN/UF2、没有 cache/DMA 一致性接口。

**P4 第一阶段范围（写死）**：
- 只用**一个 HP 核** + 片上 SRAM 做任务窗口；PSRAM 只做数据/文件缓存；第二个 HP 核与 LP 都不进 v1；
- 只做 B-5a/b/c 台阶（探针 → 出 prompt → `accept.py` 24/24）+ 单域窗口 + PMP/保护 + UART/timer；
- 不做多域窗口、不做 WiFi full stack、不做 LP 调度。

---

## 3. 目标结构

```
arch/<isa>/                 ← 架构层：frame/irq/保护/timer
board/<chip>/               ← SoC 家族：时钟/XIP/ROM/核启动/IRQ/timer
board/<vendor>/<board>/     ← 板级：引脚、外设存在性、LED/KEY/LCD
configs/<target>.conf       ← 现有 INI：能力与裁剪
kernel/                     ← 只依赖契约，不出现板级 #ifdef
drivers/<class>/upper.c     ← 协议/状态机；lower 由 board/chip 提供
abi/                        ← syscall/结构/格式的单一真相
build/gen/                  ← 构建期生成物，不入库
```

**"板子即目录"的精确含义**：
- 新板只动 `board/<vendor>/<board>/` + `configs/<target>.conf`；
- chip 目录由 SoC 家族维护，板级不得改；
- 内核不得出现 `#ifdef BOARD_X`；
- 同芯片不同板用 **variant**（ATK / Pico 2）表达。

### 3.1 源文件、生成物、跨仓库快照

"生成物不入库"只约束**派生输出**，不约束**输入**。三类东西分清楚：

| 类别 | 例子 | 入库 | 规则 |
|---|---|---|---|
| **源文件** | `configs/*.conf`、`board/*/board.conf`、`abi/syscalls.csv` | 是 | 手写、评审、带版本号；改动要单独提交 |
| **派生输出** | `build/gen/board_config.h`、生成的 syscall 头/表 | 否 | 由 `go run ./cmd/tos gen` 从源文件重建；CI 可复现；不进 diff |
| **跨仓库快照** | goc32 的 `abi/syscalls.snapshot.csv` | 是 | 是 goc32 的**输入**，不是构建产物；文件头记 `tinyos_commit` 与表 hash |

因此 goc32 里那份表**必须入库**：它不能读 TinyOS 工作区，否则发布就依赖另一个仓库的
当前状态。两个仓库同时存在时，由测试比对快照与 `abi/syscalls.csv`；只有 goc32 时，
快照自洽即可构建。这条规则对 `board.conf` 也适用：板级事实入库，生成头不入库。


---

## 4. Phase A：只加接缝，不改行为

**Phase A 结束条件：12 目标构建 + QEMU smoke/soak + 有硬件的板级基线完全不变。**

### A-1｜board.conf 能力声明 + 冲突校验（C1）
- 复用 INI，新增 `board/<vendor>/<board>/board.conf`：`schema=1`、`arch`、`cores`（HP/LP）、`memory_domains`、`protection`、`connectivity`、`peripherals`、`variant`。
- `ApplyConfig()` 在套用 conf features/drivers 前校验目标能力；不相容直接报错。
- 扩展 `geometry_test.go` 为 board.conf ↔ `layout.c` ↔ `WindowGeometry()` 三方一致。
- **不做**：生成头、搬 `layout.c`、TOML。

### A-2｜ABI 结构单一头 + syscall 表（C2）
- `include/tos_abi.h`：现有 `stat/meminfo/clockinfo/dfinfo` + `_Static_assert(sizeof)`；内核改用 `sizeof/offsetof`。
- `abi/syscalls.csv` + 生成/比对；**先不改 flags、不改结构布局**。
- **不做**：LZ4TOS 版本位（放 Phase B）。

### A-3｜exec_format 接缝（C2）
- `struct exec_format {name, probe, load, note}`；LZ4TOS 和 ELF 各注册一个；`process.c` 走 handler，旧路径兜底。
- 为 P4 新格式、goc32 产物、格式版本化提供单点。
- **不做**：新的 loader 实现；只搬现有逻辑。

### A-4｜flash 独占 + cache/DMA 钩子（C3）
- `platform.h` 加 `plat_flash_exclusive_begin/end`；RP 三块板先包一层现有 park，行为不变。
- 加 `plat_icache_sync()`、`plat_dcache_clean/invalidate(range)`、`PLAT_CACHE_LINE`；现有板 no-op。
- **不做**：通用 `plat_core_pause/resume`；不做三块板协议合并（Phase B）。

### A-5｜内存域接缝（C4）
- `struct mem_domain {base, size, grain, protect, flags}`，现在 `domains[1]`。
- `win_alloc` 加域参数；`process_user_ok`/`plat_task_protect` 走域选择，但单域行为完全一致。
- **不做**：PSRAM 第二域、cache 维护实现。

### A-6｜等待纪律（C6）
不引入 `wd_start`。这里冻结的是**规则**，不是一个定时器 API：

- **tick 就绪后**：跨硬件/跨核的等待用 `deadline = plat_ticks() + ms_to_ticks(ms)`，
  超时返回 `-ETIMEDOUT`（或驱动约定的错误）。
- **tick 就绪前**（XOSC/PLL/clock mux/UART FIFO 等早期初始化）：保留迭代上限，但必须
  按 `plat_cpu_hz()` 或板级常量折算，并注明 `pre-tick`；不得直接抄 `1e6`。`board/rp2040`
  的 `(g_cpu_hz / 3000u) * 200u` 是现成先例；`board/rp2350rv` 的 1e6 循环就是待折算样本。
- 超时是**上限**，不是延时替代：正常路径仍轮询状态位，不得为了"有超时"改成 sleep 固定
  毫秒。
- **不在持锁/关中断状态下做长等待**；LCD 的 `5e6` 轮询是第一个样本（在 `lcd_lock` 内）。
- `wd_start(cb, ticks)` 移到 B-8，触发条件：网络/companion 的请求级超时。

**不做**：全量替换 `sleep` 列表；把早期初始化改成读 tick。

### A-7｜LCD 接缝（C5）
- `struct lcd_lowops {init, write_cmd, write_data, fill_rect, sync, delay_ms}`；RP 的 ST7789 作为第一个 lower。
- 输出/时序不变；mock lower 单测加在 Phase B。
- **不做**：GPIO/SPI/WiFi 全量 upper/lower。

---

## 5. Phase B：把实现搬进去

| 项 | 内容 | 依赖/验收 |
|---|---|---|
| B-1 | 生成 `build/gen/board_config.h`，搬 `layout.c`/ld 常量 | A-1 稳定；12 目标几何不变 |
| B-2 | RP park 四件套共享骨架；内核接 `plat_flash_exclusive_*` | A-4；200 轮 dd + smpbench |
| B-3 | LZ4TOS flags 高字节版本 + V1 分支；goc32 黄金向量 | A-2/A-3；旧 `.tos` 加载 |
| B-4 | LCD upper/lower + mock 测试；P4 补 lower | A-7；显示/DMA 回归 |
| B-5 | P4 启动/镜像探针（旧 E-0）；cache/DMA 实现 | A-4；见下方 B-5a/b/c |
| B-6 | LP/companion 的窄消息接口 + 心跳 | P4 硬件；LP 不进入 SMP |
| B-7 | 多内存域（仅 PSRAM 窗口确有需求） | C4；QEMU 双段 + P4 硬件 |

#### B-5 的三级台阶

| 台阶 | 内容 | 通过标准 |
|---|---|---|
| **B-5a 探针** | vendor 最小寄存器头（时钟/GPIO/UART0）；`esptool` ROM 下载；打印版本/复位原因/时钟 | P4 上电稳定打印一行；记录 image header、偏移和 `write-flash` 命令 |
| **B-5b 最小系统** | clock + UART + timer + 中断；能出 `tos> ` prompt | 心跳/`uptime` 正确，shell 可交互 |
| **B-5c 平权** | flash + fs + 单域窗口 + PMP + `exec` | `scripts/esp-c3/accept.py`（按 P4 的 console/复位原因参数化）24/24；`priotest` 单 HP 核基线 |

"一行串口"只是 B-5a 的验收，不是 P4 第一阶段的全部。**P4 v1 的终点是 B-5c**：
不含 WiFi、LP 调度、PSRAM 可执行窗口。

**验收与止损的关系**（两者不能各说各话）：

- `accept.py` 是串口上的 shell 语义测试，不是 C3 端口绑定；但它的启动/复位断言依赖
  `RTC_SW_SYS_RST` 这类 C3 文案，P4 上要按实际复位原因与 console（UART vs USB CDC）
  参数化，这是一次小的移植，不是重写。
- 24 项里的 `prot rodata/heap/stack/null` 依赖 `plat_task_protect`。若止损点触发
  （PMP 做不到 4 KB 粒度），**B-5c 的通过标准改为 20/24**：prot 四项按设计不适用，
  降级理由写进 `OPEN-ITEMS.md`；不得把降级结果记成 24/24。
- `priotest` 只在 B-5c 用**单 HP 核**跑，作为调度器语义门槛（worst 不超过一个 tick、
  不输给 NORMAL）；不套 C3 的绝对毫秒数，也不代表 PSRAM 路径已经收口
  （PSRAM 的 `priotest` 门槛只在启用 PSRAM 窗口时才有意义，见 `OPEN-ITEMS.md` §9）。

**止损点**：B-5b 之后若确认 PMP 做不到 4 KB 粒度保护，v1 明确降级为"无用户保护"，
记入 `OPEN-ITEMS.md`，不为了跑通保护而改窗口模型。

**B-8 条件项，不排期**：5 档优先级、workqueue、tickless、task group、通用 RPC。出现具体需求再立项，且先补 PI/aging 等前提。

---

## 6. 明确不做

- Kconfig/CMake/TOML/外部代码生成器；
- 全量 POSIX、完整 ESP-Hosted、通用 RPC；
- 通用 `plat_core_pause/resume`；
- 全量驱动 upper/lower 迁移；
- P4 第一阶段 PSRAM 可执行窗口；
- 合并 msh/sh 或两份 libc；
- 为了架构好看改 12 个既有目标的运行时语义。

---

## 7. 顺序与验收

```
Phase A（可并行、每项独立提交）
  A-1 配置校验 ─┬─> A-4 flash/cache ─> A-5 内存域接缝
  A-2 ABI 头     ├─> A-3 exec_format
  A-6 等待/超时  └─> A-7 LCD lowops

Phase B（A 稳定后）
  B-1 生成头
  B-2 park 骨架 + 独占接口
  B-3 LZ4TOS 版本 + 黄金向量
  B-4 LCD upper/lower
  B-5 P4 探针
  B-6 LP/companion
  B-7 多域（条件）
```

**每项提交的验收**：
- `go build ./...`、`go vet ./...`、`go test ./... -count=1`；
- 12 目标构建（x86 按 `OPEN-ITEMS.md` §5 记录）；
- QEMU `rv32/rv32smp/arm` smoke + soak；
- 有硬件的板按 `docs/TESTING.md` 跑 T3；
- Phase A 任何一项不得改变既有目标的启动/shell/窗口/SMP 结果。

---

## 8. 新板 checklist（稳定 v1）

- [ ] `board/<vendor>/<board>/board.conf`：schema/arch/cores/memory_domains/protection/connectivity/peripherals/variant。
- [ ] `board/<vendor>/<board>/` 只写引脚/外设存在性/LED/KEY/LCD；不写时钟/XIP/驱动逻辑。
- [ ] 同芯片不同板用 variant（Pico 2 是第一个案例），不复制 `platform.c`。
- [ ] `configs/<target>.conf` 只声明能力；构建期校验。
- [ ] `scripts/accept-<board>.py` 最小心跳；纳入 CI 或 nightly。
- [ ] README 目标表按实测状态填写；未上板不写 "on hardware"。
- [ ] 不改内核 `switch`/`#ifdef`，不改 chip 目录。

---

## 9. 稳定性判据

| 指标 | 现在 | 稳定 v1 目标 |
|---|---|---|
| 配置事实 | 4 处 | 1 份 board.conf + 1 份生成头 |
| conf/target 冲突 | 静默 | 构建期明确报错 |
| 新板需改内核文件 | 多处 | 0 |
| ABI 偏移 | 内核硬编码 | 单一头 + 静态断言 |
| 格式版本 | flags 精确匹配 | 高字节版本 + V1 兼容 |
| flash 独占 | 板级私货 | 内核契约 + 共享骨架 |
| 等待 | 分散 bounded wait | 统一 `wd_start` |
| 单域→多域 | 无接缝 | `mem_domain` 列表，单域行为不变 |
| 新板验收 | 手工 | 每板心跳脚本 |

---

## 10. 开放问题

1. **P4 启动链**：ROM 偏移/header/二级 bootloader/`esptool` 是否 `write-flash 0x0`。
2. **P4 保护单元**：PMP/MMU、PSRAM 可保护性、粒度（PSRAM 的目标形态与升级门槛见
   `OPEN-ITEMS.md` §9：先做数据池，不做可执行窗口）。
3. **P4 cache/DMA**：cache line、PSRAM 一致性、bounce 需求。
4. **LP 核**：谁拉起、共享内存边界、mailbox 协议。
5. **companion**：ESP-Hosted 全栈还是窄帧接口。
6. **生成文件**：`build/gen/` 不入库；CI 可复现命令固定。
7. **Pico 2**：`PC=0xeffffffe` 根因；variant 的 LED/LCD/flash 几何；稳定前不进回归基线。
8. **goc32**：按 §3.1，goc32 提交 `abi/syscalls.snapshot.csv`（入库，带 TinyOS commit
   与 hash），发布不依赖 TinyOS 工作区；两边都在时由测试比对。

---

## 11. 对 NuttX / FreeRTOS 的取舍

- **借 NuttX**：每任务 watchdog → A-6；binfmt handler → A-3；file_operations/mount 表 → C5 的后续；workqueue → 条件项；task group → 条件项。
- **借 FreeRTOS**：mutex PI → 仅扩优先级时；stack high-water/overflow hook → 已有 `ps`/哨兵，补报告；tickless → 后期电源项。
- **不借**：Kconfig、POSIX 全套、MMU addrenv、event groups/queue sets、动态分配/多种 heap、通用 RPC。

---

## 12. 与上一版 E 编号的对应

| 上一版 | 本版 |
|---|---|
| E-0 P4 探针 | B-5 |
| E-1 配置真相 | A-1（校验）/ B-1（生成头） |
| E-2 跨核独占 | A-4（接缝）/ B-2（骨架） |
| E-3 驱动分层 | A-7（LCD 接缝）/ B-4（upper/lower） |
| E-4 ABI | A-2（头/表）/ A-3（exec_format）/ B-3（flags） |
| E-5 内存域 | A-5（接缝）/ B-7（多域） |
| E-6 LP/companion | B-6 |
| 新增 | A-6 等待/超时 |
