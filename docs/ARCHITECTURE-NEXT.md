# TinyOS 架构演进提案：稳定 v1（接缝先行）

- **状态**：提案，未实施。本文只定结构与契约，不实现具体功能。
- **基线**：当前 `main`（代码 `fea309f`，2026-11-16；本文档提交在其后）。行号会漂移，定位以函数名/符号名为准。
- **本版目标**：**一次性把稳定架构的"接缝"和"兼容承诺"定对**，实现允许分批；不追求一个大 PR 把所有板子/驱动/格式一起改完。
- **与 review 的边界**：`PROJECT-REVIEW-2026-11-16.md` 的具体小修继续由执行流处理；本文只处理跨子系统、会改变目录或接口契约的部分。

---

## 0. 什么是"稳定"

稳定不等于"代码不再变"，而是：

1. **契约稳定**：板级能力、ABI/格式、跨核独占、内存域、驱动接缝、等待语义、远端端点各有唯一入口和版本号；
2. **实现可替换**：接缝后面可以换实现（RP → P4、本地 → companion），接缝本身不变；
3. **既有目标语义不变**：12 个目标的行为、shell、窗口、SMP 性能在 Phase A 前后一致。

"一次性"指的是**一次把接缝和兼容承诺定对**，不是一次把所有实现写完。

### 0.1 必须冻结的 7 个契约

| # | 契约 | 冻结什么 | 现在给什么实现 | 以后怎么扩展 |
|---|---|---|---|---|
| C1 | **board/chip/config** | `board.conf`（INI，`schema=1`）+ `board/<chip>` / `board/<vendor>/<board>` 分层 + 能力校验 | 只声明能力 + 构建期校验；不生成头、不搬 `layout.c` | 三层事实（chip/board/image）；`Targets` 全生成、`build/gen/board_config.h`；加 variant（Pico 2） |
| C2 | **ABI / 格式** | `include/tos_abi.h`、`abi/syscalls.csv`、`exec_format`、LZ4TOS flags 版本位（V0 `0x0001` 特例，见 §0.4） | 结构/编号单一头 + 静态断言；LZ4TOS/ELF 各一个 handler | 新 syscall 追加；新格式注册 handler；flags 高字节版本 |
| C3 | **flash/跨核独占** | `plat_flash_exclusive_begin/end` + 协作式 park poll + `plat_cache_*`/`plat_dma_*` 钩子 | RP 现有 park 包一层，行为不变；cache/DMA 钩子 no-op | 三块 RP 共享骨架；P4 实现自己的独占；不承诺通用 pause |
| C4 | **内存域/窗口** | `struct mem_domain` 列表（现在 1 个）+ `win_alloc` 带域参数 | 单域，行为与现在完全一致 | P4 PSRAM 需要时加第二域；不提前做多域 |
| C5 | **驱动接缝** | `dev_ops` + LCD `lcd_lowops` | LCD 的 RP lower 接到 ops；输出不变 | P4/新板补 lower；不强制 GPIO/SPI/WiFi 全量迁移 |
| C6 | **等待/超时** | 等待**规则**（不是定时器 API）：tick 就绪后用毫秒上限；tick 前按 `plat_cpu_hz()` 折算迭代上限；不在持锁/关中断状态长等 | 规则 + 一个 helper；`wd_start` 不做 | `wd_start` 只在网络/companion 出现请求级超时时立项（B-8） |
| C7 | **远端端点** | `remote_ep_send/recv/poll`、`link_state(ep)`、`last_rx_tick`；`send/recv` 仅任务上下文；`poll`/`link_state` 在任务或 tick 上下文（可做 I/O）；`remote_ep_irq()` 是唯一 ISR 入口（只置 pending，无 I/O）；容量参数走 `remote_ep_caps()` | 形状冻结，无真 lower；先 mock + QEMU | LP 与 C6 companion 各实现一个 lower；参数不同不改契约 |

### 0.2 兼容承诺（违反一条就必须单独评审，不能混进架构提交）

- 既有 syscall 编号**只追加，不改语义**；
- 既有结构**只追加字段，不重排**；`_Static_assert(sizeof(...))` 守住；
- 旧 V0 `.tos`（`flags=0x0001`）在新内核继续加载；
- 12 个既有目标在**正常路径、合法输入**下的可观察语义不变（Phase A 的例外见 §4）；
- 每条契约各自有版本策略（见 §0.4）；扩展是加字段/加 handler，不是重写；
- 不把 Pico 2 的未解决问题带进回归基线（`OPEN-ITEMS.md` §1b）。

### 0.3 断言边界（KASSERT）

判据不是"能否由用户触发"，而是**这个值是不是本内核自己算出来的**：

- **内部不变量**（窗口 freelist 元数据、`kfree` 双释放、`proc` 状态非法迁移、
  `build_initial_stack()` 自己算出的尺寸）→ 可以 halt。
- **外部数据**（非法 ELF、畸形 note、`user_range_ok()` 失败）→ 永远返回错误；
  一个坏程序不能让内核停机。
- **配置策略**（MPU/PMP region 不够）→ 按策略降级或 fail-closed，不是断言。

实现纪律：

- `KASSERT(cond)` 走 `panic()`；`KASSERT_HALT(cond)` 走 `plat_panic_halt()`
  （不打印、ISR 可用）。不做运行时"我在不在 ISR"的猜测，调用点自己选。
- **覆盖率看收口点，不看断言条数**：`kfree_locked()` 现有的
  `if (!(b->size & ALLOC_BIT)) return;` 是唯一收口，改成 halt 就覆盖全部调用者；
  `proc` 先加 `proc_set_state()`，**1 个点覆盖现有 27 处直接赋值**。
- **收口点落地前先 grep 证明没有旁路**：另一条 free 路径、直接写 `->state`、宏封装。
  "唯一入口"是这套论证的全部效力，旁路一旦存在，"一个点覆盖全部"就不成立；
  审计结果写进 commit（本仓库当前：`kfree` 无旁路，`->state` 有 27 处直接写）。
- 仅仅"值得记一笔"的偏差（栈尺寸与预算不符）用计数器，不 halt。
- `TOS_KASSERT=0` 可按 target 关闭（bring-up 用），**默认开**；`_Static_assert` 永远开。
- halt 点位逐个过审，判据是"命中即意味着状态不可恢复"，不是"看起来可疑"。

### 0.4 冻结判据与版本策略

**冻结判据**：冻**形状与调用约束**（改则牵动所有调用点），不冻**未测参数**（猜错就是 bug）。
C4（单域形状冻、第二域参数不冻）与 C7（`send/recv/link_state` 形状冻、
`max_frame`/`queue_depth` 由 `remote_ep_caps()` 返回）都按这一条判。

| 项 | 现在 | 目标 |
|---|---|---|
| `board.conf` | `schema=1` | 追加键只升 schema |
| `tos_abi` | 只有 `sizeof`/`offsetof` 断言，无版本字段 | A-2 加 `TOS_ABI_VERSION`；结构只追加 |
| LZ4TOS flags | 内核接受 V0（`0x0001`）与 V1（`0x0101`）；`mklfs` 写 V1，goc32 仍写 V0（作为旧分支样本） | 新能力位只放可忽略项；改变加载语义必须升版本 |
| `exec_format` | `{name, probe, load, note}`，无版本 | B-3 追加 `version` 字段；旧 handler 不动 |

"带版本号"在这张表里曾是**目标态**；`board.conf` 的 `schema=1` 与 LZ4TOS 的 V1
（`0x0101`，`mklfs` 已开始写）现在都是真字段；`tos_abi` 的版本字段仍待 A-2 后续、
`exec_format` 的 `version` 仍待 B-3 的第二节。

LZ4TOS 的格式版本直接等于 flags 高字节，名字不再和编码值错位：

- **V0** = 现有 `0x0001`（bit0 = hint，没有版本字段，历史格式）；
- **V1** = 第一个带版本字段的格式，`flags = (1 << 8) | caps`，即 `0x0101`；
- 判格式只看 flags 高字节，不做"文档名 - 1"这类映射。

`LZ4_WRAP_HEADER_V1`（`kernel/lz4.h`）是**28 字节带 hint 头**的名字，与格式版本号无关，
**不改名**；注释里已写明这一点。

`caps` 只放**可以安全忽略**的能力提示（hint 是第一个）；任何"忽略就会加载错"的变化必须
升编码版本，不能靠 caps 位。兼容承诺只覆盖一个方向：**新内核读旧文件**；旧内核读到新
文件不承诺能识别，C2/B-3 的验收按这个方向写。

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
- 新板只动 `board/<vendor>/<board>/` + `configs/<target>.conf` + `internal/build` 的
  `Targets` 表一行（B-1 之后这一行由 `board.conf` 生成）；
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

## 4. Phase A：先修 bug + 加接缝，不改正常路径语义

**"不改行为"的准确含义**：合法输入、正常路径下的可观察语义（启动、shell、窗口几何、
SMP 调度正确性）不变。已批准的例外，每项仍要附自己的回归证据：

- (a) 修既有真 bug：A-0 把并发 exec 从"可能加载错文件/越界写"改成正确；
- (b) 内部不变量从静默容忍改 halt：A-8，依 §0.3；
- (c) 等待循环按主频折算：A-6，"等到状态就绪"的语义不变，只是上限可换算。

**Phase A 结束条件：12 目标构建 + QEMU smoke/soak + 有硬件的板级基线完全不变**
（按上面的定义判，不按字面"一个字节都不变"判）。

### A-0｜SMP exec 并发（先做，BUG-A/B）
放行条件：这是真 bug，不是接缝；先修完再动 A-3，否则 loader 带病重构。

- `kernel/elf.c`：`g_win_lo/g_win_hi` 移进 `elf_view_t`（`lo/hi` 由 `win`/`win_size`
  派生），删 `elf_set_window()` 与 `elf.h` 的声明；`user_range_ok/zero_user/read_u32/
  write_u32` 带 `const elf_view_t *`。
- `kernel/syscall.c::do_exec`：`static char path[]` 改栈上（256 B，最小 8 KiB 内核栈的 3%）。
- `kernel/ramdisk.c` 的静态缓冲已有 `fs_lock`，**不动**；把它改成 per-call 是拆已验证设计。
- 验收：QEMU `rv32smp` 两核并发 exec soak（可复现）+ 真机；校验进程名与加载内容一致。
  回帖写清驱动 soak 的程序和命令行。

### A-1｜board.conf 能力声明 + 冲突校验（C1）
- 复用 INI，新增 `board/<vendor>/<board>/board.conf`：`schema=1`、`chip=<型号>`、
  `peripherals`（外设存在性）、`connectivity`、`variant`、引脚/LED/KEY 事实。
- **chip 事实不在 board.conf 里**：`arch`/`triple`/`march`/时钟/XIP/核数/保护后端/
  内存域由 chip 层唯一拥有；board.conf 只写 `chip=<型号>`，构建期据此解析并校验。
  写 `arch=` 会把同一事实放两个地方，正是 B-1 警告的那种两层真相。
- `ApplyConfig()` 在套用 conf features/drivers 前校验目标能力；不相容直接报错。
- 能力校验的覆盖范围要说清楚：`featurePeripheral()` 目前只映射 `wifi`/`pio`/`lcd`；
  其余 feature 暂时静默通过，A-1 的注释里写明，或改成按 `peripherals` 反推。
- `chip=` 的校验**不要按板名硬编码**：用"已知 chip 集合 + chip→arch 映射"，再核对
  `Target.Arch`；测试遍历 `Targets` 里的棋盘，而不是写死板名列表。否则"加一块已有 chip
  的新板"仍要改三处 Go（`Targets`、chip 映射、测试），正好是 C1 要消灭的返工面。
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

### A-8｜KASSERT 落地（C2 的配套）
按 §0.3 执行，放在 A-0/A-3 之后：

- `kfree_locked()` 的静默 `return` 改成 halt（收口点已存在，一行改动覆盖全部调用者）；
- 新增 `proc_set_state()` + 合法性表，替换现有 27 处直接 `->state =`；
- `TOS_KASSERT=0/1` 开关（默认开）、`KASSERT`/`KASSERT_HALT` 两种形态；
- `_Static_assert` 与 `offsetof` 断言随 A-2 一起，永远开。

**不做**：给每个调用点加断言；在外部数据路径上 panic。

---

## 5. Phase B：把实现搬进去

| 项 | 内容 | 依赖/验收 |
|---|---|---|
| B-1 | 生成 `build/gen/board_config.h`，搬 `layout.c`/ld 常量 | A-1 稳定；12 目标几何不变；见下方三层事实 |
| B-2 | RP park 四件套共享骨架；内核接 `plat_flash_exclusive_*` | A-4；200 轮 dd + smpbench |
| B-3 | LZ4TOS flags 高字节版本 + V0 分支；goc32 黄金向量 | A-2/A-3；旧 `.tos` 加载 |
| B-4 | LCD upper/lower + mock 测试；P4 补 lower | A-7；显示/DMA 回归 |
| B-5 | P4 启动/镜像探针（旧 E-0）；cache/DMA 实现 | A-4；见下方 B-5a/b/c |
| B-6 | LP/companion 的窄消息接口 + 心跳（C7 形状） | mock lower 在 QEMU 跑通；LP 不进入 SMP |
| B-7 | 多内存域（仅 PSRAM 窗口确有需求） | C4；QEMU 双段 + P4 硬件 |

#### B-1 的三层事实（生成边界）

生成之前先把事实分层，否则只是把"两层真相"从 Go 挪进 INI：

| 层 | 放什么 | 在哪 |
|---|---|---|
| chip | arch/triple/march/时钟/XIP/内核段布局 | chip 目录固定事实，不随板变 |
| board | 引脚、外设存在性、LED/LCD/PSRAM、驱动能力、硬件不存在的用户程序 | `board/<vendor>/<board>/board.conf` |
| image | heap/max_procs/kstack/opt、本镜像裁掉的用户程序 | `configs/<target>.conf` |

- `Targets` **全部**成为生成物。`Drivers` 不写成路径：board.conf 写能力
  （`peripherals = gpio, spi, lcd, usb, pio`），能力→源码目录的映射留在 Go 的共享表里，
  这样加板不改 Go。
- `UserExclude` 拆成两个语义：board.conf 的"硬件不存在"（永久，例如 RP2040 无 MPU →
  `prot` 不编）与 configs 的"本镜像不带"（可逆）。混成一个字段就会出现
  "某镜像裁了 net、另一个镜像又带回来"的假真相。
- `layout.c` / `kernel.ld` **留源文件**：`checkLayout()` 继续解析 `boot/stage1.S` 与
  `board/*/layout.c`；读生成物会让生成器写错时"自洽通过"。生成物只配往返测试
  （生成两次一致 + 与源文件解析值相等）。

**B-1a 已落地（2026-11-17）**：`UserExclude` 的"硬件不存在"列表已从 `Targets`
移到 `board.conf [user] exclude`（`prot` 只在 RP2040 上排除；WiFi 程序在四块 RP 板上
排除）；`configs` 的排除仍是可逆的"本镜像不带"。改前/改后用 `configs/rp2040.conf`
构建的 ramdisk 字节相同（HISTORY §47）。剩余：chip/arch/驱动列表/镜像默认值仍在 Go，
B-1b 处理。

**B-1b 已落地（2026-11-17）**：

- `internal/build/chip.go` 保存 chip→ISA 的 triple/`-mcpu`（RP2350 同时有 arm/rv32）；
  `board.conf` 的 `chip=` 改为查表校验，`ensureBoardConfig()` 拒绝 target 与 chip 的
  triple/mcpu 不一致（`-march` 仍属于 target：rv32 与 rv32nc 是同一 hart 的镜像选择）。
- 每次构建从 `board.conf` 生成 `build/gen/board_config.h`（board/chip/variant、窗口三数、
  `TOS_HAS_*` 外设位），只由内核消费。
- `kernel/board_check.c` 在启动时比较生成值与 `layout.c` 常量，不一致直接 panic；
  已用故意把 `qemu-virt` 的 guard 改成 8k 验证会触发（HISTORY §48）。
- 剩余 B-1c：把 `Targets` 的 chip/arch/驱动目录/镜像默认值也从 conf/chip 表派生，
  届时 `Targets` 只剩"这块板有哪些镜像"（单核/双核）这一层。

**B-1c 已落地（2026-11-17）**：`Targets` 只剩 board/arch/march/cpus 与镜像默认值
（heap/maxprocs/kstack）；triple/`-mcpu` 来自 chip 表，fs_backing/ramdisk_sectors/
xip_flash_base/drivers/default_features 来自 `board.conf`，由 `applyBoard()` 填充；
`TargetByName()` 给没有 Builder 的调用方（menuconfig）用。构建产物与 B-1b **字节相同**
（rp2040 ramdisk、C3 full，HISTORY §49）。剩余：镜像清单（单核/双核）仍是 Go 字面量，
若以后加板需要，B-1d 可从一份 targets 清单生成。

#### B-5 的三级台阶

| 台阶 | 内容 | 通过标准 |
|---|---|---|
| **B-5a 探针** | vendor 最小寄存器头（时钟/GPIO/UART0）；`esptool` ROM 下载；打印版本/复位原因/时钟 | P4 上电稳定打印一行；记录 image header、偏移和 `write-flash` 命令 |
| **B-5b 最小系统** | clock + UART + timer + 中断；能出 `tos> ` prompt | 心跳/`uptime` 正确，shell 可交互 |
| **B-5c 平权** | flash + fs + 单域窗口 + PMP + `exec` | `accept.py` 24/24（reboot 的 reset-cause 文案先抓后填）；`priotest` 三条判据（见下） |

"一行串口"只是 B-5a 的验收，不是 P4 第一阶段的全部。**P4 v1 的终点是 B-5c**：
不含 WiFi、LP 调度、PSRAM 可执行窗口。

**验收与止损的关系**（两者不能各说各话）：

- `accept.py` 是串口上的 shell 语义测试，不是 C3 端口绑定；但 `reboot resets the chip`
  这一项写死了 C3 的 ROM 复位原因文案（`RTC_SW_SYS_RST`），其余 23 项是通用的
  prompt-send-expect 检查。P4 上**先跑一次 `reboot` 抓到实际的 reset-cause 字符串**，
  再把它和 console 类型（UART vs USB CDC）一起参数化；不能直接沿用 C3 的期望，
  否则该项会以"文案不匹配"失败，看起来像 reboot 坏了。
- 24 项里的 `prot rodata/heap/stack/null` 依赖 `plat_task_protect`。若止损点触发
  （PMP 做不到 4 KB 粒度），**B-5c 的通过标准改为 20/24**：prot 四项按设计不适用，
  降级理由写进 `OPEN-ITEMS.md`；不得把降级结果记成 24/24。
- `priotest` 只在 B-5c 用**单 HP 核**跑。判据照 `scripts/esp-c3/priotest.py` 的实现，
  不写"看起来还行"：
  1. 每个 hog 数下，`nice 0` 的 miss 数不劣于 `normal`；
  2. `nice 0` 的 worst gap ≤ 2 ms（脚本判 `rt[4] > 2` 即失败）；
  3. hog ≥ 2 时，`nice 0` 的 miss 数 ≤ 30/1000。

  注意脚本里那句 "order of magnitude fewer misses" 的注释与它实际执行的
  `rt_bad > 30` 并不完全一致；**引用判据时以代码为准，不以注释为准**。
  这些阈值不套 C3 的绝对毫秒数，也不代表 PSRAM 路径已经收口
  （PSRAM 的 `priotest` 门槛只在启用 PSRAM 窗口时才有意义，见 `OPEN-ITEMS.md` §9）。

**止损点**：B-5b 之后若确认 PMP 做不到 4 KB 粒度保护，v1 明确降级为"无用户保护"，
记入 `OPEN-ITEMS.md`，不为了跑通保护而改窗口模型。

#### B-6 的纪律（C7 形状已冻结，参数未定）

- 内核里第一个符号必须是 `remote_ep_*`，不得出现 `lp_mailbox_*`；LP 专属名字只允许在
  lower 里。
- `link_state(ep)` 是查询；liveness 是 `last_rx_tick` 属性，**不做 `heartbeat()` 命令**
  （那会逼出上层定时器，重复造 liveness）；`recv` 只做轮询 + 超时，不先写死回调。
- `remote_ep_irq(ep)` 是唯一的 ISR 入口：只置 pending 位/计数，无 I/O、无锁。
  ISR **不唤醒任务**——`process_tick()` 调用 `remote_ep_poll(ep)` 排空 pending、
  更新 `link_state` 后再唤醒，所以不存在"盲唤醒"，也不需要 lower 在 ISR 里可读。
  `link_state()` 因此是任务/ tick 上下文，C6 的 lower 可以在里面做 SPI 查询。
- `send/recv` **只允许任务上下文**；ISR 只能走 `remote_ep_irq()`。这条现在定，晚了改的是所有调用点。
- `max_frame`/`queue_depth` 由 `remote_ep_caps()` 返回，**不写成宏**：LP 的 mailbox 与
  C6 的 SPI 帧容量不同，mock 证明不了；猜错冻进去的是 bug 不是接口。
- 先做 mock lower + QEMU 的 send/recv 测试，等 B-5b 拿到 LP 真实行为后再评容量与错误码。

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
  A-0 SMP exec 并发（BUG-A/B，先做）
  A-1 配置校验 ─┬─> A-4 flash/cache ─> A-5 内存域接缝
  A-2 ABI 头     ├─> A-3 exec_format ─> A-8 KASSERT
  A-6 等待纪律   └─> A-7 LCD lowops

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
- **复现/压力类测试必须记命令行**（不是只写"跑过了"），并在
  `docs/acceptance/` 留一份摘要；
- Phase A 任何一项不得改变既有目标在正常路径、合法输入下的启动/shell/窗口/SMP 结果
  （例外见 §4）。

**心跳级六项的适用范围**：只用于**新板接入**和 **P4 的 B-5b/B-5c**；Phase A/B 的每项提交
按上面的 T0–T2 跑，不要求心跳。已有板（C3 / ATK / RP2040）的 T3 是**平权级**；
心跳级通过不等于板子可交付（`docs/TESTING.md` §5.0）。

---

## 8. 新板 checklist（稳定 v1）

- [ ] `board/<vendor>/<board>/board.conf`：schema/chip/peripherals/connectivity/variant + 引脚/LED 事实；chip 事实（arch/时钟/核数/保护后端）不写在这里。
- [ ] `board/<vendor>/<board>/` 只写引脚/外设存在性/LED/KEY/LCD；不写时钟/XIP/驱动逻辑。
- [ ] 同芯片不同板用 variant（Pico 2 是 variant 机制的**动机案例**；它的 HardFault 另案，稳定前不进基线），不复制 `platform.c`。
- [ ] `configs/<target>.conf` 只声明能力；构建期校验。
- [ ] 在 `internal/build` 的 `Targets` 表登记一行——这是当前唯一必须改的 Go 源码，
      B-1 之后交给 `board.conf` 生成；不要假装它已经消失。
- [ ] `scripts/<board>/heartbeat.py`：按 `docs/TESTING.md` §5.0 的心跳级六项，末行
      `HEARTBEAT OK n/n`、退出码 0/1；脚本随 target 入库，纳入 CI 或 nightly。
- [ ] 平权验收写 **24/24 或具名 N/24**；"构建期不存在"与"运行期降级"分开记，
      两者都进 `OPEN-ITEMS.md`，禁止"按能力取子集"和把 N/24 记成 24/24。
- [ ] README 目标表按实测状态填写；未上板不写 "on hardware"。
- [ ] 不改内核 `switch`/`#ifdef`，不改 chip 目录。

---

## 9. 稳定性判据

| 指标 | 现在 | 稳定 v1 目标 |
|---|---|---|
| 配置事实 | 4 处 | chip/board/image 三层各一处，`Targets` 全生成 |
| conf/target 冲突 | 静默 | 构建期明确报错 |
| 新板需改内核文件 | 多处 | 0（`Targets` 表一行在 B-1 后消失） |
| ABI 偏移 | 内核硬编码 | 单一头 + `offsetof` 静态断言 |
| 格式版本 | flags 精确匹配 | 高字节版本 + V0 兼容 |
| flash 独占 | 板级私货 | 内核契约 + 共享骨架 |
| 等待 | 迭代数当时钟，部分在锁内 | tick 后毫秒上限、tick 前按主频折算；不在锁内长等 |
| 断言 | `assert()` 无条件 no-op | 内部不变量 halt、外部数据返回错误；`TOS_KASSERT` 默认开 |
| 远端端点 | 无 | C7 形状 + mock lower（参数由 caps 提供） |
| 单域→多域 | 无接缝 | `mem_domain` 列表，单域行为不变 |
| 新板验收 | 手工 | 心跳级 6 项（`scripts/<board>/heartbeat.py`） |
| 平权验收 | 24 项全过或口头子集 | 24/24 或具名 N/24，两种不入项都要 OPEN-ITEMS |

---

## 10. 开放问题

1. **P4 启动链**：ROM 偏移/header/二级 bootloader/`esptool` 是否 `write-flash 0x0`。
2. **P4 保护单元**：PMP/MMU、PSRAM 可保护性、粒度（PSRAM 的目标形态与升级门槛见
   `OPEN-ITEMS.md` §9：先做数据池，不做可执行窗口）。
3. **P4 cache/DMA**：cache line、PSRAM 一致性、bounce 需求。
4. **LP 核**：谁拉起、共享内存边界、mailbox 协议（接口形状见 C7；容量参数未定）。
5. **companion**：ESP-Hosted 全栈还是窄帧接口（C7 只冻结消息形状与任务上下文约束）。
6. **生成文件**：`build/gen/` 不入库；CI 可复现命令固定。
7. **Pico 2**：`PC=0xeffffffe` 根因；variant 的 LED/LCD/flash 几何；稳定前不进回归基线。
8. **goc32**：按 §3.1，goc32 提交 `abi/syscalls.snapshot.csv`（入库，带 TinyOS commit
   与 hash），发布不依赖 TinyOS 工作区；两边都在时由测试比对。

---

## 11. 对 NuttX / FreeRTOS 的取舍

- **借 NuttX**：每任务 watchdog 只借"请求级超时"这一半 → B-8；`binfmt` handler → A-3；file_operations/mount 表 → C5 的后续；workqueue → 条件项；task group → 条件项。
- **借 FreeRTOS**：`configASSERT` 的开关形态 → §0.3 的 `TOS_KASSERT`（但默认开）；mutex PI → 仅扩优先级时；stack high-water/overflow hook → 已有 `ps`/哨兵，补报告；tickless → 后期电源项。
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
| 新增 | A-0 SMP exec、A-6 等待纪律、A-8 KASSERT、C7 远端端点（形状） |
