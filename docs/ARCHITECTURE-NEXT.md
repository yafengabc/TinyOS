# TinyOS 架构演进提案：面向多板扩展（修订版）

- **状态**：提案，未实施。本文只谈结构与扩展机制，不修复具体缺陷。
- **基线**：当前 `main`（`b2bfb38`，2026-11-16）。上一版基线是 `923a55d`；工作区若有未提交改动，行号会漂移，定位时以函数名和符号名为准。
- **与并行工作的边界**：`PROJECT-REVIEW-2026-11-16.md` 里的具体小步修复（几何、SysTick、park 超时、`wdt_feed`、死代码、热路径、ABI 硬编码等）由执行流单独处理；本文只处理**需要跨子系统、会改变目录或接口契约、必须成体系做**的演进。
- **本版修订依据**：最近一轮 review 修复已经落地（几何 4K、N-01/N-02/N-03、N-04/N-05、CI Go 检查）；goc32 已初始化并自带汇编/链接器；Pico 2 失败已立为 `OPEN-ITEMS.md` §1b。下列"现状"按这个基线重写。

---

## 0. 不变量（改架构时不能破坏）

1. **无外部构建系统**：构建只需要 Go + clang + ld.lld，不得引入 Kconfig、CMake、外部代码生成器作为前置。**仓库内 `go run ./cmd/tos` 自带的生成步骤是允许的**；生成物不入库。
2. **板级差异声明化，芯片逻辑共享**：一块板的差异放在**一个 board 目录**里（引脚、外设存在性、LED/KEY/LCD）；SoC 家族逻辑（时钟、XIP、ROM、核启动、IRQ/timer）放在 chip 目录，**新板不得改 chip 目录或内核 `switch`/`#ifdef`**。这是对"板子即目录"的精确化，不是把差异拆到两个目录。
3. **msh 体积预算**：`user/msh/msh.c` 在 rp2040 只剩几十字节余量。任何用户态"共享库"式重构若把 msh 拉出 16 KiB 窗口，直接否决。内核侧新增代码另设预算，不能拿 msh 的预算当借口。
4. **早期初始化时序**：`plat_early_init()` 在堆、devfs、进程表、回调都不存在时运行；早期层只能用编译期/链接期绑定。
5. **既有目标运行时语义零变化**：12 个目标的可观测行为（启动、shell、窗口、SMP 性能）保持一致；新语义只允许出现在新板/新 target 上，且必须显式选择。

---

## 1. 现状再评估（相对上一版）

| 条目 | 上一版描述 | 现在 | 对提案的影响 |
|---|---|---|---|
| 窗口几何 | `WindowGeometry()` 返回 16K/8K/4K，与 `layout.c` 4K/4K/4K 漂移 | **已修**：返回 4K/4K/4K，新增 `geometry_test.go` | E-1 的第一段"先止损"已部分完成；剩下的是配置冲突和单一真相 |
| N-01/N-02/N-03 | SysTick 优先级回退、RV park 超时关中断、`wdt_feed` 未驻 RAM | **已修**（`19352a4`） | E-2 仍值得做，但动机从"修 bug"转为"防止再次分叉" |
| N-04/N-05 | 信号/唤醒状态在锁外 | **已修**（`d577ab8`、`fa5b276`） | 与 E-2 无关，单独回归 |
| CI Go 检查 | 没有 `go test`/`go vet` | **已加**（`82a3c53`） | E-1/E-4 的验收可以进 CI |
| goc32 | 未初始化，无法验证 | **已初始化且自带汇编/链接器**（`7a17826`），feature 宏从 `build-config-<target>.txt` 导入 | E-4 的黄金向量现在可做；上一版"先 init"的开放问题关闭 |
| Pico 2 | 未记录 | **`OPEN-ITEMS.md` §1b**：v0.3.0/本轮 `rp2350smp` 早期 HardFault（`PC=0xeffffffe`），MicroPython 正常 | E-1 必须支持**同芯片不同板**的 variant；`board/rp2350` 不能假设 ATK 的 LED/LCD/4MiB |
| `-target` 与 conf 冲突 | 静默套用 conf 的 features | **仍存在**（`ApplyConfig()` 只在命令行未选 target 时使用 conf target，之后仍套 features/drivers） | E-1a 是最先要做的 |
| flash park | 三块板各一套，内核只有无参 poll | 仍如此（修复了超时，但协议未收敛） | E-2 保留 |
| ABI 布局 | `stat/meminfo/...` 只在 `user/lib.h`，内核写死偏移 | 仍如此 | E-4 保留 |
| 内存域 | 单一连续窗口区 | 仍如此 | E-5 保持条件性 |

结论：**E-1、E-2、E-4 仍然成立，但 E-1 要先做"校验"而不是"生成"；E-5 继续后置；新增 P4 启动链和 cache/DMA 两个前置项。**

---

## 2. P4 为什么是分水岭

官方规格（Espressif 产品页/新闻稿，2026-11-16）：**双 HP RISC-V 核（400 MHz）+ 单 LP 核（40 MHz）**，768 KB 片上 L2MEM，8 KB TCM，最多 32 MB 封装内 PSRAM，**无片内无线**（靠 ESP32-C6 companion），带 MIPI-CSI/DSI、H.264、USB 2.0 HS。

用户已明确：**LP 核按协处理器处理，不进 SMP 调度器**。NuttX 的 SMP 文档也要求同构处理器池，异构核不进 `CONFIG_SMP_NCPUS`。

P4 同时挑战五个当前假设：

| 假设 | 现状 | P4 现实 |
|---|---|---|
| 核是对等的 n 个 | `cpu_count/cpu_init` 一视同仁 | 2 HP + 1 LP，速度差 10 倍 |
| 内存是单一连续域 | `win_alloc` 在 `[PLAT_APP_BASE, PLAT_APP_MAX)` 切 | SRAM + cache 后的 PSRAM，两个域 |
| 外设是本地 MMIO | `plat_gpio_*` 等 weak 直接操作寄存器 | WiFi 在 companion，走 SPI/SDIO |
| 启动链是 PICOBIN/UF2 | RP2350 的 image_def + UF2 | P4 的 ROM 头/偏移/二级 boot 未确认 |
| 没有 cache/DMA 一致性接口 | 现有板无 cache 维护钩子 | PSRAM + DMA 必须 clean/invalidate |

新增板子分四类，代价递增：

1. 同芯片不同板（Pico 2 vs ATK）——当前 `board/rp2350` 混在一起，已出问题。
2. 同芯片不同架构端口（rp2350/rp2350rv）——约 500 行驱动逐字重复。
3. 不同芯片同 ISA（C3/P4）——`board/esp32c3/platform.c` 几乎不可复用。
4. 新核拓扑（P4 2 HP + LP）——超出当前 SMP 抽象。

---

## 3. 目标结构（按本项目规模裁剪）

```
arch/<isa>/                 ← 架构层：frame/irq/保护/timer（保持现状）
board/<chip>/               ← SoC 家族层：时钟/XIP/ROM/核启动/IRQ/timer
board/<vendor>/<board>/     ← 板级层：引脚、外设存在性、LED/KEY/LCD
configs/<target>.conf       ← 用现有 INI 声明能力与裁剪，不引入 TOML
kernel/                     ← 只依赖上面生成的接口，不出现板级 #ifdef
drivers/<class>/upper.c     ← 协议/状态机；lower 由 board/chip 提供
build/gen/                  ← 构建期生成的头/表，不入库
```

**明确不抄 NuttX 的部分**：POSIX 全套、Kconfig 工具链、强制 devicetree、通用 RPC 框架。只学数据流形态，不学工具。

**"板子即目录"的精确含义**：
- 新板只动 `board/<vendor>/<board>/` 和 `configs/<target>.conf`；
- chip 目录由 SoC 家族维护，板级不得改；
- 内核不得出现 `#ifdef BOARD_X`；
- 同芯片不同板通过 **variant** 表达（Pico 2 vs ATK 是第一个例子）。

---

## 4. 改进项（依赖顺序）

### E-0｜P4 启动/镜像探针（新增，先于所有架构项）

**动机**：E-1..E-5 都在讲运行时；P4 首先要知道 ROM 从哪读 image、header/分段格式、是否需要二级 bootloader、`esptool` 怎么写。这是 P4 最大不确定性，也是所有架构讨论的前提。

**目标**：一个只打印一行串口的最小 P4 镜像，确认启动链和工具链，再谈抽象。

**步骤**：
1. vendor 最小 P4 寄存器头（时钟/GPIO/UART0）；不引入 ESP-IDF。
2. 用 `esptool` 的 ROM 下载模式把探针放进 SRAM/IRAM 运行；确认 `write-flash` 的偏移/头格式。
3. 给 `internal/build` 抽一个 `ImageFormat` 接口；C3 的 `espimage.go` 是第一个实现，P4 是新实现，不共用 C3 魔数。
4. 探针输出固件版本/复位原因/时钟/串口，作为后续 bring-up 的基线。

**验收**：P4 上电后串口稳定打印一行；记录 image header/偏移/`esptool` 命令；不改 12 个既有目标。

**风险**：如果 ROM 要求签名/二级 bootloader，必须决定"自写最小 stage-2"还是 vendored bootloader；这个决定单独评审。

---

### E-1｜能力声明与配置单一真相源

**分两步，先校验后生成。**

#### E-1a：能力声明 + 冲突校验（先做）

- 复用现有 INI 格式，新增 `board/<vendor>/<board>/board.conf`（不引 TOML）：
  - `arch`、`cores`（HP/LP 及角色）、`memory_domains`（基址/大小/是否 cache 后/保护后端）、`protection`（MPU/PMP/none + 粒度）、`connectivity`（本地/companion）、`peripherals`（存在的外设类）、`variant`。
- `ApplyConfig()` 在套用 conf 的 features/drivers/user-exclude 前，校验与目标能力相容；不相容直接报错并列出冲突项。
- 现有 `geometry_test.go` 扩展为"board.conf ↔ layout.c ↔ WindowGeometry()"三方一致性测试。
- **不搬 `layout.c`、不生成头**。

**验收**：
- `-target rp2040 -config configs/esp32c3-wifi.conf` 构建失败并说明"rp2040: no wifi capability"。
- 每个 board 的 geometry/RAM/FS 常量一致；`go test ./...` 全绿。
- 12 目标 smoke 不变。

#### E-1b：生成唯一配置头（后做）

- 从同一 `board.conf` 生成 `build/gen/board_config.h`（不入库）；`layout.c`/链接脚本的常量改为引用生成头。
- C 编译包含 `build/gen`；Go 侧读同一结构，不再各写一份。
- 生成物可由 `go run ./cmd/tos gen` 单独重建，便于 CI 排查。

**风险**：新增间接层；板级工程师改几何的入口从 C 文件变成 board.conf，必须写进 `board/README.md`。先只做 E-1a，验证无回归再搬常量。

#### E-1c：board variant

- `rp2350` chip 下支持 `atk-dnrp2350am` 和 `pico2` 两个 variant；LED（GPIO3 vs GPIO25）、LCD 存在性、flash 大小/文件系统块数由 variant 决定。
- Pico 2 必须先解决 `OPEN-ITEMS.md` §1b 的早期 HardFault（当前 `PC=0xeffffffe`），再纳入回归矩阵；`board/rp2350/README.md` 里"同一套代码也跑 Pico 2"的旧说法要改。

---

### E-2｜跨核独占与 flash park 契约（收窄）

**不再追求通用 `plat_core_pause/resume`。** RP2350 的 park 是协作式的：另一核必须在 idle/spin/block 入口轮询；用户态死循环且关中断时无法暂停。通用 pause 在 P4 上会逼出 IPI + 安全点机制，风险远大于收益。

**目标**：
- 在内核可见的层面定义 **flash/XIP 独占区间**：`plat_flash_exclusive_begin()` / `plat_flash_exclusive_end()`；
- 板级保留协作式 `plat_flash_park_poll()`，但把三块 RP 板的 park/unpark 四件套抽成共享骨架（宏注入 IRQ/屏障/WDT/NOTIFY），不把整套协议复制三份；
- 若未来确实需要"暂停另一核"，单独提案，明确 cooperative/timeout/不可暂停的场景，不混进 flash 契约。

**步骤**：
1. `platform.h` 加成对接口；内核在 block 擦写入口包住；idle/spin 轮询点统一调用。
2. 抽 `board/rp_park_protocol.h`（宏/内联），三块 RP 板只留架构原语和调用序列。
3. SMP 目标链接期检查：`plat_flash_exclusive_begin/end` 不得落到 weak no-op；单核板可以 no-op。
4. LP 核不参与这套机制；它的通信走 E-6。

**验收**：三块板 200 轮 `dd`+`rm`、`smpbench` 与基线一致；内核源码不再出现 `g_park_req/g_park_ack` 等板级符号；静态检查 SMP 目标无 weak 默认。

---

### E-3｜驱动 upper/lower 分层（先 LCD，不推全量）

**目标**：LCD 试点，证明形态；GPIO/SPI/WiFi 不强制迁移。

- `drivers/display/st7789/upper.c`：命令序列、framebuffer 抽象、`/dev/lcd0`/`/dev/fb0` 注册；
- `struct lcd_lowops {init, write_cmd, write_data, fill_rect, sync, delay_ms}`；rp2350 ARM/RV32 各一份 lower，共享 90% 代码；
- `kernel/periph.c` 的 weak 保留作为单板快速实现的兼容层；新板走 ops；
- **早期/晚期分层**：任何在 `plat_early_init()` 使用的驱动不能依赖 ops 注册或堆；LCD 属于晚期，可以走 ops。

**明确不做**：
- 不把 `remote_ops` 做成通用 RPC；WiFi companion 先定义窄的 net device + 帧传输；
- P4 的 LP 核按"远程处理器"单独立项（E-6），不塞进 WiFi 驱动层。

**验收**：LCD DMA/显示与基线一致；mock lower 单测覆盖命令序列和边界；msh 目标码不增；内核 size 有预算记录。

---

### E-4｜ABI 与格式单一真相源

**分三步，先无运行时影响。**

#### E-4a：结构布局单一头

- 把 `struct stat/meminfo/clockinfo/dfinfo` 收敛到 `include/tos_abi.h`，带 `_Static_assert(sizeof(...)==N)`；
- 内核改用 `sizeof/offsetof` 写用户内存，不再硬编码偏移；
- 用户 `lib.h` 包含同一头；保留现有大小/字段顺序。

#### E-4b：syscall 编号表

- `abi/syscalls.csv`：`name,id,arg_count,struct_layout_version`；
- 内核、`internal/build`、goc32 从同一表生成/比对；不再手写数字；
- **goc32 是独立仓库**：生成表要么随 goc32 提交快照，要么由 `-tos` 指定目录读取；发布 goc32 不能依赖 TinyOS 工作区，需在 goc32 仓库保留一份可验证的 golden 表。

#### E-4c：LZ4TOS flags 版本化

- flags 改"高字节版本 + 低字节能力位"：`flags & 0x0001` 判 hint、`flags >> 8` 分派解析器；保留 V1 分支；
- 旧 `.tos` 继续加载；新内核遇到未知版返回明确错误；**旧内核无法给新格式明确错误**，只能被旧逻辑拒绝，这一点文档要写清。

**验收**：旧 V1 `.tos` 在新内核加载；新 flags 在新内核有明确错误；`tos build` 与 goc32 的黄金向量在 probe/note/load 三处一致。

---

### E-5｜内存域抽象（条件性，P4 第一阶段不做）

**P4 第一阶段明确只用 768 KB 片上 SRAM 做任务窗口**；PSRAM 先作为数据堆/文件缓存，不做可执行窗口。理由：
- P4 的 PMP/PSRAM 保护粒度未确认；
- cache 后 PSRAM 的 DMA 一致性会牵动所有驱动；
- 先用单域跑通 boot/console/timer/PMP/exec，才能判断是否真的需要 PSRAM 窗口。

如果确实需要：
1. 从 E-1 的 `memory_domains` 生成 `const struct mem_domain domains[]`；
2. `win_alloc` 增加域参数；`process_user_ok`/`plat_task_protect` 按域选保护后端；
3. 配 `plat_dma_prepare/complete` 和 cache 维护；
4. 先在 QEMU 模拟两个连续段，再上 P4。

**前置**：P4 PMP/PSRAM datasheet 确认；未确认前不写代码。

---

### E-6｜远程核与 companion（新增，P4 相关）

- **LP 核**：按协处理器建模，定义启动链、mailbox/共享内存、消息格式；不注册进 `cpu_count`/SMP。
- **WiFi companion（C6）**：只定义窄传输接口（帧 + 控制），复用现有 mailbox/ttyraw 帧格式；不在 TinyOS 内实现 ESP-Hosted 全栈。
- 与 E-3 的 `lcd_lowops` 无关；不要为了 WiFi 把驱动层做成通用 RPC。

---

## 5. 落地批次

| 批次 | 内容 | 前置/验收 |
|---|---|---|
| **0** | E-0 P4 启动探针；E-1a 能力声明+冲突校验；E-4a 公共 ABI 头；cache/DMA 钩子设计 | 不改现有语义；12 构建 + QEMU smoke |
| **1** | E-1b 生成头/搬常量；E-2a RP park 共享骨架；E-4b syscall 表；LZ4TOS flags 版本 | 三块 RP 板 200 轮 dd；旧 `.tos` 兼容 |
| **2** | E-2b `plat_flash_exclusive_*`；E-3 LCD 试点；E-6 LP 探针 | LCD 显示/DMA 回归；P4 LP 心跳 |
| **3** | E-5 内存域（仅在 PSRAM 窗口确有需求时）；E-3 推广到 WiFi/GPIO | P4 硬件确认；否则不做 |

每批结束：12 目标构建 + QEMU smoke/soak + 有硬件的板级验收；任一批回归不通过，不进下一批。

---

## 6. 新板接入 checklist（修订）

- [ ] `board/<vendor>/<board>/board.conf` 声明 arch/cores/memory_domains/protection/connectivity/peripherals/variant；P4 的 LP 写 `role=coprocessor`。
- [ ] `board/<vendor>/<board>/` 只写引脚、外设存在性、LED/KEY/LCD；不写时钟/XIP/驱动逻辑。
- [ ] 同芯片不同板用 variant 表达（Pico 2 是第一个案例）；不复制整份 `platform.c`。
- [ ] `configs/<target>.conf` 只声明要哪些能力；构建期校验相容性。
- [ ] `scripts/accept-<board>.py` 最小心跳验收（烧录→固定命令→比对输出→内存域/保护测试），纳入 CI 或 nightly。
- [ ] `README.md` 目标表按实测状态更新；未上板不写"on hardware"。
- [ ] 新板不修改内核 `switch`/`#ifdef`，不修改 chip 目录。

---

## 7. 度量

| 指标 | 上一版基线 | 现在/目标 |
|---|---|---|
| 几何漂移 | 16K vs 4K | **已关闭**（4K + geometry_test） |
| 配置事实存放处 | 4 处 | 1 份 board.conf + 1 份生成头（分两步） |
| conf/target 冲突 | 静默套用 | 构建期明确报错 |
| 同芯片新架构端口重复 | ~500 行 | 0（共享驱动 + 架构 memmap） |
| 新板需改内核文件数 | 多处 | 0 |
| "一个端口修了另一个没修" | 多次 | 协议/接口单一来源后结构性减少 |
| P4 启动链 | 未知 | E-0 探针给出 header/偏移/tool 命令 |
| 新板验收 | 手工脚本 | 每板一个心跳脚本 |

---

## 8. 开放问题（动手前回答）

1. **P4 启动链**：ROM 从哪个偏移读、header 格式、是否要二级 bootloader、`esptool` 是否支持 `write-flash 0x0`。
2. **P4 保护单元**：PMP 还是 MMU？PSRAM 能否保护、粒度多少？（阻塞 E-5）
3. **P4 cache/DMA**：cache line 大小、PSRAM 一致性、DMA 是否需要 bounce/维护。
4. **LP 核**：谁拉起、跑什么、共享内存边界；与 HP 的 mailbox 协议。
5. **companion**：ESP-Hosted 是否要在 TinyOS 内实现，还是复用现有帧格式做窄接口。
6. **生成文件策略**：`build/gen/` 不入库；CI artifact 需要可复现，生成命令要固定。
7. **Pico 2**：`OPEN-ITEMS.md` §1b 的 `PC=0xeffffffe` 根因；variant 的 LED/LCD/flash 几何；在它稳定前不作为回归基线。
8. **goc32**：已初始化；E-4b 的 golden 表在 goc32 仓库如何保留快照，避免发布依赖 TinyOS 工作区。

---

## 9. 明确不做

- Kconfig/CMake/TOML 依赖、外部代码生成器；
- 全量 POSIX、完整 ESP-Hosted、通用 RPC 框架；
- 通用 `plat_core_pause/resume`（除非单独提案并证明可协作暂停）；
- P4 第一阶段启用 PSRAM 可执行窗口；
- 合并 msh/sh 或两份 libc（见 review 附录 D）；
- 为了"架构好看"改动 12 个既有目标的运行时语义。
