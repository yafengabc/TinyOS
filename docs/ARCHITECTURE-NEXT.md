# TinyOS 架构演进提案：面向多板扩展

- **状态**：提案，未实施。本文只谈**结构与扩展机制**，不修复具体缺陷。
- **基线**：提交 `923a55d`（含 `PROJECT-REVIEW-2026-11-16.md`）。行号以该提交为准；工作区若有未提交改动，行号会漂移，定位时以**函数名和符号名**为准。
- **与并行工作的边界**：`PROJECT-REVIEW-2026-11-16.md` 的 N-01~N-07、冗余附录 A/B/C、架构附录 F/G/H/I 中**具体、小步、可立即修**的项（几何 4K、SysTick 钩子、park 超时恢复、`wdt_feed` RAM 驻留、死代码、热路径改写、一致性测试等）由另一个执行流处理，**本文不重复**。本文处理的是那些**需要跨子系统改动、会改变目录结构或接口契约、必须成体系做**的架构演进。
- **目标读者**：接下来负责架构落地的 agent。每条都给出"现状证据 → 目标 → 步骤 → 验收 → 风险"，可直接拆成任务。

---

## 0. 先立不变量（改架构时不能破坏的东西）

任何条目若与以下五条冲突，以这五条为准，宁可不做。

1. **无 SDK**：构建只需要 Go + clang + ld.lld。禁止引入 Kconfig 工具链、CMake 依赖、代码生成器作为构建前置。
2. **板子即目录**：一块板子的所有差异应集中在一个目录；新增板子不应要求改内核 C 文件的 `switch` 或散布的 `#ifdef`。
3. **msh 体积预算**：`user/msh/msh.c` 在 rp2040 只剩几十字节余量。任何"共享库"式重构若把 msh 拉出 16 KiB 窗口，直接否决。这也是 msh/sh 刻意重复的原因。
4. **早期初始化时序**：`plat_early_init()` 在堆、devfs、进程表、回调都还不存在时运行。早期层只能用编译期/链接期绑定，不能依赖运行时注册或分配。
5. **运行时语义零变化**：本文所有条目完成后，12 个目标的可观测行为（启动、shell、窗口分配、SMP 性能）应保持一致；几何、调度、保护逻辑不改算法。

---

## 1. 为什么现在要动架构：P4 是分水岭

官方规格（Espressif 新闻稿与产品页，2026-11-16 查证）：ESP32-P4 是**双 HP RISC-V 核（400 MHz）+ 单 LP 核（40 MHz）**，768 KB 片上 L2MEM（可配成 PSRAM 的 cache）、8 KB TCM、最多 32 MB 封装内 PSRAM；**无片内无线**，靠 ESP32-C6 经 SPI/SDIO 做 companion；带 MIPI-CSI/DSI、H.264 编码、PPA、USB 2.0 HS。

用户已明确：**LP 核按协处理器处理，不进 SMP 调度器**。这与 NuttX 的立场一致——NuttX 的 SMP 文档明确要求是 "pool of homogeneous processors"，异构核不进 `CONFIG_SMP_NCPUS`。

P4 因此同时挑战三个当前隐含假设：

| 假设 | 现状代码 | P4 的现实 |
|---|---|---|
| 核是对等的 n 个 | `cpu_count`/`cpu_init()` 把 secondary 一视同仁（`kernel/cpu.c:117-161`） | 2 HP + 1 LP，速度差 10 倍 |
| 内存是单一连续物理区间 | `win_alloc` 在 `[PLAT_APP_BASE, PLAT_APP_MAX)` 按 grain 切 | 片上 SRAM + cache 后的 PSRAM，两个域 |
| 外设是本地 MMIO | `plat_gpio_*` 等 weak 函数直接操作寄存器（`kernel/periph.c:31-152`） | WiFi 在 companion 芯片上，走 RPC |

新增板子分四类，代价递增；本文要解决的是后三类的结构性成本：

1. 同芯片不同板 —— 当前已 OK（同 board 目录 + `layout.c`）。
2. 同芯片不同架构端口 —— rp2350/rp2350rv 约 500 行驱动逐字重复，已两次分叉出 bug。
3. 不同芯片同 ISA —— C3 与 P4 同为 RISC-V，`board/esp32c3/platform.c` 几乎无法复用。
4. 新核拓扑 —— P4 的双 HP + LP，超出当前 SMP 抽象。

**病根一句话**：隐含约束（配置几何、早期时序、weak 接口的真实依赖、ABI 布局、flash 独占协议）没有编码成可验证的契约，全靠注释和人记，于是必然漂移、必然"一个端口修了另一个没修"。

---

## 2. 目标架构（借鉴 NuttX，但按本项目规模裁剪）

NuttX 的做法（官方文档已查证，2026-11-16）：

- `arch/<arch>/` 放处理器架构，`arch/<arch>/src/<chip>/` 放 SoC 特定逻辑，`boards/<arch>/<chip>/<board>/` 放板级，另有硬规则："THERE SHOULD BE NO APPLICATION CALLABLE LOGIC IN THIS DIRECTORY"。
- 配置：`defconfig → .config → include/nuttx/config.h`，全系统只包含一个配置头；板级能力用 Kconfig 的 `select` 声明（如 `select ARCH_LED`），不是运行时猜。
- 驱动 upper/lower half：upper 注册到内核（`register_driver`）、实现高层接口，**通过回调调 lower**；lower 硬件相关，放 arch/chip 或 board。已有 `ipcc/`、`rptun/`（Remoteproc Tunnel）、`usrsock/` 承担跨芯片通道。
- SMP：`up_cpu_pause()/up_cpu_resume()/up_cpu_start()` 是 arch 层正式 API；线程有 affinity mask（`sched_setaffinity()`）。
- 符号表：`syscall.csv`/`libc.csv` 是 ABI 单一真相源，`tools/mksymtab` 生成符号表。

**TinyOS 的目标态**（按本项目规模裁剪，不要照搬重量级机制）：

```
board/<chip>/                 ← SoC 家族层：寄存器/时钟/XIP/核启动，芯片相关但架构无关
     rp2350/                     （rp2040 自成一类；esp32c3、esp32p4 各一类）
arch/<isa>/                   ← 架构层：frame/irq/protection/timer，保持现状
board/<vendor>/<board>/       ← 板级层：引脚映射、外设存在性、LED/KEY/LCD（现 board_devs.c 的职责）
configs/<target>.conf         ← 声明"这个目标要什么"（能力 + 裁剪），构建期生成唯一配置头
kernel/                       ← 只依赖上面生成的接口，不出现板级 #ifdef
drivers/<class>/upper.c       ← 协议/状态机与板无关的部分（先试点 LCD，再 WiFi）
```

**明确不抄**：POSIX 全套（msh 体积预算撑不住）、Kconfig 工具链本身（破坏"只需 Go+clang"）、devicetree 强制化（规模不需要）。**学数据流形态，不学工具**。

---

## 3. 改进项（按依赖顺序，每项可独立拆任务）

### E-1｜能力声明与配置单一真相源

**动机**：修完几何漂移（另一个执行流已把 `WindowGeometry()` 改成 4K）只是堵住一个症状。根因是 `Targets` 表（`internal/build/build.go:38-255`）、`configs/*.conf`、`board/*/layout.c`、链接脚本四份各存一部分事实，且 `-target` 与 conf target 冲突时静默套用 conf 的 features（`internal/build/config.go:285-370`）。

**目标**：板级"能力"（核数、内存域、保护模型、有哪些外设、无线是否 companion）在**一处**声明；构建期生成唯一配置头；Go 侧和 C 侧都读生成结果，不再各写各的。

**步骤**：
1. 设计 `board/<chip>/board.toml`（或复用现有 INI 格式）：字段至少含 `arch`、`cores`（逻辑核及角色：hp/lp）、`memory_domains`（基址/大小/是否 cache 后/保护后端）、`protection`（mpu/pmp/none + 粒度）、`connectivity`（本地无线 / companion + 通道）、`peripherals`（存在的外设类）。
2. 构建期读清单，生成两个产物（**不入库**，每次构建生成，避免第四份真相）：
   - `build/gen/board_config.h`：`#define TOS_BOARD_HAS_WIFI 0` 等编译期常量；
   - 内存中的 Go 结构，供 `WindowGeometry()`、FS 布局校验、`checkWindow()` 使用。
3. `internal/build/config.go` 的 `ApplyConfig()` 在套用 conf 的 features 前，校验 conf 所需的 capability 与目标声明是否相容；不相容直接报错并列出冲突项（例：`-target rp2040 -config esp32c3-wifi.conf` → "rp2040: no wifi capability"）。
4. 逐步把 `board/*/layout.c` 的几何/RAM/FS 常量改为引用生成头（仍是 `const uint32_t` 或宏，**不改运行时读取方式**），链接脚本的 `ALIGN`/`ASSERT` 字面量由清单注入。

**验收**：
- `go test ./...` 全绿，且新增表驱动测试覆盖：每个 board 清单与对应 `layout.c` 的值一致；每个 conf 与每个 target 的相容性矩阵。
- `tos build -target rp2040 -config configs/esp32c3-wifi.conf` 必须**构建失败**并给出明确冲突信息。
- 12 个目标的现有 smoke 全部通过，镜像大小波动 < 1%。

**风险/回退**：清单格式是新增间接层，板级工程师改几何的入口从 C 文件变成清单——必须在 `configs/README.md` 或新 `board/README.md` 写明。生成物绝不入库。可分两步：先只加能力声明 + 一致性测试（不搬 `layout.c`），验证无回归后再搬常量。

---

### E-2｜跨核控制提升为架构接口

**动机**：SMP flash park 的四件套在 `board/rp2040/platform.c`、`rp2350/platform.c`、`rp2350rv/platform.c` 各一份约 430 行同构代码；内核只有一个无参轮询钩子 `plat_flash_park_poll`（`kernel/cpu.c:218`），不知道临界区边界。这个结构已经产出多个缺陷（见 main review N-02/N-03）。

**目标**：仿 NuttX `up_cpu_pause/resume`，把"暂停另一核 / 恢复"提升为 `platform.h` 的正式成对接口；板级只提供原语，不持有整套协议。

**步骤**：
1. 在 `kernel/platform.h` 增加：
   - `plat_core_pause(int cpu)` / `plat_core_resume(int cpu)`：语义为"让指定核对外部事件停止响应，直到 resume"；
   - `plat_core_in_critical_section(void)`：供 idle/spin 轮询点判断当前是否处于独占区。
2. 把三块板 flash 路径里"mask 中断 → 发布 `g_park_req` → 等逐代 `g_park_ack` → ROM 调用 → XIP 恢复 → unpark → restore"的骨架提到共享头（如 `board/rp_park_protocol.h`），用宏注入 `IRQ_SAVE/IRQ_RESTORE/BARRIER/WDT_FEED/NOTIFY`。
3. 内核在 spin 等待（`kernel/cpu.c:301`）、idle（`arch/arm/platform.c:527` 等）、block 操作入口统一调用新接口；删除散落的板级私有轮询约定。
4. **LP 核不参与**：P4 的 LP 核走独立协处理器通道，不注册为 SMP CPU；E-5 会处理其通信。

**验收**：
- 三块板的 200 轮 `dd`+`rm` 压力与 `smpbench` 结果与基线一致（rp2350smp 曾 40 轮 0 楔死、2.00x）。
- `plat_core_pause/resume` 在所有 SMP 板都有强实现；单核板可提供 weak no-op，但必须有构建期检查确保 SMP 目标不落到 weak 默认。
- 静态检查：内核源码不再出现 `g_park_req`/`g_park_ack` 等板级符号。

**风险/回退**：触及 spin/tick/block 三条热路径，必须在 E-1 的稳定基线上做，且分两步——先抽共享骨架（不改接口），再提升接口。每步单独回归。

---

### E-3｜驱动 upper/lower half 分层

**动机**：`kernel/periph.c:31-152` 的 17 个 weak 函数把"协议/范围检查"和"硬件操作"揉在一起；同一外设多板实现靠目录约定（`drivers/<class>/<board>/`），且 rp2350/rp2350rv 的 GPIO/SPI/board_devs 逐字重复。P4 的 WiFi 在 companion 芯片上，现有模型无法表达"下层不是本地寄存器"。

**目标**：每个外设类拆成与板无关的 upper half（注册 `/dev` 节点、实现 read/write/ioctl、参数校验）和可替换的 lower half（ops 结构 + 回调）；lower half 可以是本地 MMIO，也可以是 RPC 到 companion。

**步骤**：
1. 选 **LCD** 作为试点（已有 `st7789` 驱动，板级差异集中在 `lcd_delay_ms` 和 DMA 通道）：
   - upper half 放 `drivers/display/st7789/upper.c`：命令/初始化序列、framebuffer 抽象、`/dev/lcd0`/`/dev/fb0` 注册；
   - lower half 定义 `struct lcd_lowops {init, write_cmd, write_data, fill_rect, sync, delay_ms}`；
   - rp2350 ARM/RV32 各提供一份 lower half，共享 90% 代码。
2. 把 `kernel/periph.c` 的 weak 实现改成"upper half 检查 + 调 lower ops"的形态；保留 weak 函数作为单板快速实现的兼容层，但新板必须走 ops 注册。
3. 定义 `struct remote_ops`（基于现有 mailbox/IPC 原语），作为 companion lower half 的传输层：P4 的 WiFi、未来的 LP 核服务都挂在这层。

**验收**：
- `rp2350`/`rp2350rv` 的 LCD DMA 测试与显示输出与基线一致；msh 镜像大小不增。
- 新增一个"假 lower half"单测：upper half 配 mock ops，验证命令序列与边界检查，不依赖硬件。
- 文档化新外设接入步骤（见第 5 节 checklist）。

**风险/回退**：upper/lower 拆分会短暂增加代码体积（多一层间接调用）。对 msh 这类紧预算程序，允许保留"直接调用 lower"的快路径（编译期选择），不强制所有程序走新框架。LCD 试点成功前不推广到 GPIO/SPI。

---

### E-4｜ABI 单一真相源

**动机**：`include/tos.h:6-73` 的编号是单一来源（做得好），但 `struct stat/meminfo/clockinfo/dfinfo` 只定义在 `user/lib.h:117-209`，内核硬编码偏移写用户内存（`kernel/syscall.c:512-519` 写死 "8 bytes with type at offset 4"）。LZ4TOS 有魔数无版本、flags 精确匹配（`kernel/lz4.c:26-37`）。格式定义在内核头/解析、`tools/mklfs.c`、`internal/build/build.go`、goc32 子模块至少五处手工同步，且 goc32 当前未 init、无法验证。

**目标**：仿 NuttX `syscall.csv`，让 ABI 编号、结构布局、容器格式各有一个机器可读真相源；构建工具与编译器都从它生成，版本号成为格式的一部分。

**步骤**：
1. 新增 `abi/syscalls.csv`：列 `name, id, arg_count, struct_layout_version`；`internal/build` 与 goc32 都从这里生成各自的编号表。禁止在两处手写数字。
2. 把 `stat/meminfo/clockinfo/dfinfo` 布局收敛到 `include/tos_abi.h`，带 `_Static_assert(sizeof(...)==N)`；内核改用 `sizeof/offsetof` 写用户内存，不再硬编码。
3. LZ4TOS flags 改"高字节版本 + 低字节能力位"，`flags & 0x0001` 判 hint、`flags >> 8` 分派解析器，**保留 V1 分支**让存量 28B 文件继续加载。
4. 建立跨工具黄金向量：同一组程序由 `tos build` 和 goc32 各自产出 blob，内核 probe/note/load 三者结果必须一致；纳入 CI（需先 init goc32 子模块）。

**验收**：
- 新增 syscall 只需改 CSV + 一处 case，用户头文件由生成步骤产出。
- 旧 V1 `.tos` 文件能被新内核加载；新版本 flags 被旧内核拒载时有明确错误（不是静默错位）。
- `go generate`/构建脚本产出与手写版本逐字节一致。

**风险/回退**：结构布局改动影响所有用户程序，必须整包重建 FS 镜像；先做 CSV 和 `_Static_assert`（无运行时影响），最后做 flags 版本化的兼容分支。

---

### E-5｜内存域抽象（P4 前置，最后做）

**动机**：`win_alloc`（`kernel/process.c:1298-1349`）与窗口计算（`:1854-1952`）假设单一连续物理区间和统一 grain。P4 的 768 KB L2MEM + 32 MB PSRAM 是两个性质的域，且 PSRAM 在 cache 后，涉及 DMA 一致性；当前 PMP 4K 粒度未必覆盖 PSRAM 域。

**目标**：窗口分配器参数化为"内存域列表"；每个域带自己的 grain、保护后端、DMA 一致性规则。现有板子只有一个域，行为完全不变。

**步骤**：
1. 从 E-1 的 board 清单读 `memory_domains`；内核侧生成 `const struct mem_domain domains[]`。
2. `win_alloc` 增加域参数；`process_user_ok`、`plat_task_protect` 按域选择保护后端。
3. 补 DMA 一致性钩子（`plat_dma_prepare/complete`），在 PSRAM 域上做 cache 维护，本地 SRAM 域为 no-op。
4. **P4 保护单元需先查 datasheet 确认**：是否 PMP、粒度、PSRAM 是否可Protected —— 这是本条的前置阻塞项，未确认前不得写代码。

**验收**：
- 单域板子的窗口分配、prot heap/prot stack、`deep 3/6` 行为与基线一致。
- 双域配置在 QEMU 上（模拟两个连续段）跑通窗口分配与保护测试，再上 P4 硬件。

**风险/回退**：**这是本提案里风险最高的一条**，触及内存保护核心。必须在 E-1/E-3 稳定、且 P4 硬件规格确认后才启动。若 P4 只是"PSRAM 当大号 SRAM"也满足需求，可以推迟到真正需要时再做——不要为假想需求重构。

---

## 4. 依赖顺序与可并行性

```
E-1 能力声明/配置真相 ──┬──> E-2 跨核接口 ──> E-3 驱动分层 ──> E-5 内存域
   (先做，其它都依赖它)  └──> E-4 ABI 真相源 (可并行，独立)
```

- **第一批**：E-1 的能力声明 + 一致性测试部分（不改 `layout.c`），E-4 的 CSV + `_Static_assert` 部分。两者互不依赖，可并行。
- **第二批**：E-2 抽共享骨架（不改内核接口）、E-1 搬常量到生成头。
- **第三批**：E-2 提升接口、E-3 LCD 试点。
- **第四批**：E-4 flags 版本化、E-3 推广到 WiFi/GPIO。
- **第五批（条件性）**：E-5，仅在 P4 确实要上且规格确认后。

每批结束后必须跑：12 目标构建 + QEMU smoke/soak + 有硬件的板级验收。任一批回归不通过，不进下一批。

---

## 5. 新板接入 checklist（改造后应是什么样）

以 P4 为例，落地后接一块新板应该是：

- [ ] `board/<chip>/board.toml` 写清 arch/cores/memory_domains/protection/connectivity/peripherals；P4 的 LP 声明为 `role=coprocessor`，不进 SMP CPU 集合。
- [ ] `board/<vendor>/<board>/` 只写引脚映射与外设存在性（现 `board_devs.c` 的内容），不写时钟/XIP/驱动逻辑。
- [ ] companion 通道（P4→C6）实现为 lower half 的 `remote_ops`，复用 E-3 的传输层；WiFi upper half 与 C3 共用。
- [ ] `configs/<target>.conf` 只声明要哪些能力，构建期自动校验相容性；不再有"给 rp2040 配 esp32c3-wifi.conf 静默通过"。
- [ ] 新增 `scripts/accept-<board>.py` 最小心跳验收（烧录→固定命令→比对输出→内存域测试），纳入 CI；这是多板规模化的真正瓶颈。
- [ ] `README.md` 的目标表加一行，状态按实测填写（不要预支 "on hardware"）。

**不该发生**：改内核 C 文件的 `switch`/`#ifdef` 来适配新板；复制整份 `platform.c`；在 board 目录里放应用可调用逻辑（NuttX 的硬规则）。

---

## 6. 度量：怎么知道架构改进有效

| 指标 | 当前基线 | 目标 |
|---|---|---|
| 同芯片新架构端口的重复代码 | ~500 行逐字重复 | 0（共享单份驱动 + 架构 memmap） |
| 新增板子需要修改的内核文件数 | 多处 `#ifdef`/`switch` | 0 |
| 配置事实的存放处 | 4 处 | 1 处清单 + 1 处生成头 |
| "一个端口修了另一个没修"类缺陷 | 已发生多次 | 协议单一来源后结构性消除 |
| 板级配置错误的发现时机 | 运行时/链接失败 | 构建期明确报错 |
| 新板最小验收 | 手工脚本 | 每板一个 CI 心跳脚本 |

---

## 7. 开放问题（动手前必须先回答）

1. **P4 保护单元**：是 PMP 还是 MMU？PSRAM 域能否被保护、粒度多少？——阻塞 E-5，需查 datasheet 或实测。
2. **LP 核启动链**：由谁拉起、跑什么代码、与 HP 的共享内存边界在哪？——影响 E-2 的"非 SMP 核"建模。
3. **companion 通道选型**：ESP-Hosted 走 SPI/SDIO 的协议栈是否要在 TinyOS 内实现，还是复用现有 ttyraw 帧格式？——影响 E-3 的 `remote_ops` 设计。
4. **goc32 子模块**：当前未 init；E-4 的黄金向量测试需要它，请先 `git submodule update --init goc32`。
5. **生成文件是否入库**：本提案明确"不入库"，但 CI 的 artifact 需要可复现——确认构建产物路径（建议 `build/gen/`）与 `.gitignore`。

---

## 8. 与 NuttX 的对应关系（速查）

| 你的痛点 | NuttX 的做法 | 本提案条目 | 裁剪理由 |
|---|---|---|---|
| 配置散在 4 处 | `defconfig → .config → config.h` | E-1 | 不引 Kconfig 工具，只学数据流 |
| 板级能力靠猜 | Kconfig `select ARCH_LED` 等 | E-1 | 用 conf `[caps]` 段替代 |
| 外设多板重复 | upper/lower half + 回调 | E-3 | 先试点 LCD，不全量改 |
| companion 芯片 | `ipcc/`、`rptun/`、`usrsock/` | E-3 | 复用现有 mailbox 原语 |
| flash park 下沉 | `up_cpu_pause/resume` | E-2 | 提升为 arch API |
| 异构核 | SMP 要求同构 + affinity mask | E-2/E-5 | LP 不进 SMP |
| ABI 手工同步 | `syscall.csv` + mksymtab | E-4 | CSV 单一源 + 生成器 |
| 格式无版本 | 无直接对应，属本项目自研 | E-4 | 保留 V1 分支 |
| 多板回归成本 | 300+ 板由社区 CI 覆盖 | 第 5 节 checklist | 每板最小心跳脚本 |
