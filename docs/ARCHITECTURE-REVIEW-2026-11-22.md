# TinyOS 架构审查报告

- **日期**：2026-11-22
- **审查范围**：内核、架构/板级适配、驱动、用户态加载、文件系统、网络、构建系统、测试与 CI
- **审查方式**：源码结构与调用链审查、文档决策核对、定量文件规模统计、Go 基线验证、成熟嵌入式/操作系统项目对照
- **验证命令**：`go test ./...`、`go vet ./...`，两者均通过
- **变更说明**：本报告只新增审查文档，不修改实现代码
- **结论口径**：区分“当前缺陷/架构债”“已明确推迟的设计取舍”“未来触发条件”，不把项目文档已经决定暂不实现的能力一律当成缺陷

---

## 1. 执行摘要

TinyOS 已经从早期单体教学内核演进为一个支持多架构、多板卡、SMP、用户态任务窗口、MPU/PMP、动态 ELF/LZ4TOS 加载、littlefs、WiFi 和多构建目标的小型操作系统。当前最有价值的架构资产包括：

1. `arch / board / drivers / kernel / user` 的基础目录分层已经建立。
2. `targets/*.conf`、`board.conf`、chip 表和 image 配置已形成三层事实模型。
3. ABI 编号、结构体、exec format、LZ4TOS 版本、goc32 快照已经有明确契约。
4. `devfs`、`hwdev`、`mem_domain`、`exec_format`、`remote_ep` 等接缝已经出现。
5. CI 已包含 Go 测试、vet、全目标镜像构建、QEMU smoke、宿主工具和发布流程。
6. 文档对不采用 Kconfig、Devicetree、完整 VFS、完整 POSIX、tickless 等选择有明确理由，不是无意识缺失。

但随着目标从 12 个增加到 18 个，并引入 SMP、WiFi、XIP、PSRAM 预留、RP2350 双 ISA、goc32/rv32emu 等能力，早期为小内核设计的宽平台接口和巨型翻译单元开始成为主要约束。

### 1.1 最重要的 5 个架构问题

| 优先级 | 问题 | 当前影响 | 成熟项目参考 |
|---|---|---|---|
| P0 | [platform.h](file:///d:/projects/TinyOS/kernel/platform.h) 仍是宽上帝接口，板级 `platform.c` 过大 | 驱动、板级、内核边界缺少编译期强制；新板继续往大文件堆逻辑 | Zephyr driver model、NuttX upper/lower-half、FreeRTOS portable |
| P0 | [process.c](file:///d:/projects/TinyOS/kernel/process.c) 约 3,784 行，承载调度、进程、FD、exec、窗口、线程等多职责 | 修改任一核心机制都触碰高风险文件；SMP 和线程继续增加复杂度 | xv6 模块拆分、NuttX sched/task/fs/binfmt 分离 |
| P1 | WiFi 芯片驱动、自研 IPv4/TCP/DHCP/DNS、设备接口和后台任务集中在 [wifi_net.c](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_net.c) | 第二块网卡、socket、多连接或协议复用会困难 | Zephyr `net_if`/socket、NuttX lwIP/VFS |
| P1 | littlefs、块设备、打开文件表、bounce buffer、readdir 快照仍由 [ramdisk.c](file:///d:/projects/TinyOS/kernel/ramdisk.c) 直连 | 第二文件系统或第二存储后端出现时会牵动 syscall/FD/设备路径 | NuttX VFS/MTD、Zephyr FS/flash/disk 接口 |
| P1 | C 内核缺少 host-native 单元测试层 | 调度、pipe、ELF、窗口等核心逻辑主要依赖 QEMU 端到端，边界用例反馈慢 | Zephyr native_sim/twister、NuttX sim |

### 1.2 总体评分

| 维度 | 评分 | 评价 |
|---|---:|---|
| 多架构可移植性 | 7.5/10 | 架构、板级和配置分层清晰，但宽 `plat_*` 接口仍把多类能力放在一起 |
| 模块边界 | 5.5/10 | 目录边界存在，include 和依赖方向尚未被构建规则强制 |
| 调度/SMP | 6.5/10 | 已支持 per-CPU runqueue、状态机和锁纪律，但调度器仍在巨型 `process.c`，高级实时能力有限 |
| 驱动模型 | 6/10 | `devfs/hwdev` 已可工作，LCD 也有 upper/lower 试点，但缺统一设备生命周期/电源状态 |
| 内存保护 | 7/10 | 任务窗口 + PMP/MPU/none 后端实用，`mem_domain` 接缝已建立；当前仍是单可执行域 |
| 存储架构 | 6/10 | littlefs 集成扎实、性能优化有证据，但 VFS/MTD seam 尚未抽象 |
| 网络架构 | 4.5/10 | 能支撑当前 ESP32-C3 场景，但协议栈、驱动和设备节点耦合较深 |
| 构建/配置 | 7.5/10 | chip/board/image 三层事实已明显改善，仍有大型 Builder 和多语言格式解析重复 |
| 测试/CI | 7.5/10 | CI 已较完整；缺 host-native C 测试和系统化 fuzz/corpus 入口 |
| 文档与决策可追溯性 | 8/10 | 架构取舍、测试级别、开放项和成熟项目对照记录充分 |

**总体判断**：TinyOS 当前不缺“能跑的功能”，主要缺的是把已经长出来的能力收口成稳定、可替换、可测试的窄接口。建议下一阶段以“无 ABI 行为变化的接缝拆分”为主，而不是继续叠加新硬件或新协议。

---

## 2. 当前架构地图

### 2.1 运行时分层

```text
user/
  用户程序、libc/syscall 包装、shell 和网络工具
    ↓ ABI: include/tos_abi.h + abi/syscalls.csv
kernel/syscall.c
  参数复制、路径规范化、系统调用分发
    ↓
kernel/process.c
  proc_t、任务窗口、FD、调度、exec、线程、wait/exit
    ↓
kernel/{elf,lz4,ramdisk,pipe,mailbox,mem_domain,devfs,hwdev,...}
  加载、文件系统、IPC、内存域、设备和外围服务
    ↓
kernel/platform.h 契约
    ↓
arch/<isa>/ + board/<board>/ + drivers/<class>/
  陷阱、IRQ、定时器、PMP/MPU、Flash、控制台、GPIO/I2C/SPI/USB/LCD/WiFi
```

### 2.2 构建时分层

```text
targets/*.conf
  image 策略：board、arch、CPU 数、heap、max_procs、kstack
        ↓
board/<board>/board.conf
  板级事实：chip、peripherals、fs_backing、窗口几何、flash 几何、驱动能力
        ↓
internal/build/chip.go
  chip 事实：triple、march、ISA、XIP、芯片能力
        ↓
internal/build
  Builder、编译、链接、littlefs、UF2/ESP image、QEMU、测试工具
        ↓
build/gen/board_config.h
  生成物，只作为内核输入，不作为手写事实源
```

该方向是正确的。[targets.go](file:///d:/projects/TinyOS/internal/build/targets.go#L11-L80) 已从目录加载目标并处理重复/别名冲突；[boardconf.go](file:///d:/projects/TinyOS/internal/build/boardconf.go#L11-L54) 明确把 board facts 与 image policy 分开；[ARCHITECTURE-NEXT.md](file:///d:/projects/TinyOS/docs/ARCHITECTURE-NEXT.md#L25-L35) 也把这些列为冻结契约。

---

## 3. 定量证据

### 3.1 最大的第一方源文件

| 行数 | 文件 | 主要职责 |
|---:|---|---|
| 3,784 | [process.c](file:///d:/projects/TinyOS/kernel/process.c) | 调度、进程、线程、FD、exec、窗口、wait/exit |
| 2,114 | [rp2350-generic/platform.c](file:///d:/projects/TinyOS/board/rp2350-generic/platform.c) | RP2350 ARM 板级平台实现 |
| 2,005 | [rp2350rv-generic/platform.c](file:///d:/projects/TinyOS/board/rp2350rv-generic/platform.c) | RP2350 RISC-V 板级平台实现 |
| 1,756 | [build.go](file:///d:/projects/TinyOS/internal/build/build.go) | 编译、链接、用户程序、FS、镜像编排 |
| 1,559 | [wifi_net.c](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_net.c) | IPv4/ARP/ICMP/DHCP/DNS/TCP/HTTP 与 `/dev/net` |
| 1,435 | [rpi-pico/platform.c](file:///d:/projects/TinyOS/board/rpi-pico/platform.c) | RP2040 板级平台实现 |
| 1,202 | [arm/platform.c](file:///d:/projects/TinyOS/arch/arm/platform.c) | ARM 陷阱、MPU、调度切换 |
| 1,048 | [wifi_osi.c](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_osi.c) | WiFi OSI/后台任务适配 |
| 958 | [esp32c3-generic/platform.c](file:///d:/projects/TinyOS/board/esp32c3-generic/platform.c) | ESP32-C3 板级平台实现 |
| 908 | [ramdisk.c](file:///d:/projects/TinyOS/kernel/ramdisk.c) | littlefs、块设备、打开文件表、readdir |

### 3.2 平台接口规模

- [platform.h](file:///d:/projects/TinyOS/kernel/platform.h) 中约有 **78 个 `plat_*` 函数声明**。
- 全仓 C 文件中约有 **1,311 处 `plat_*` 调用**，分布在 56 个 C 文件。
- 内核自身约 497 处，板级和驱动约 800 处。

这说明 `plat_*` 已不只是“架构移植层”，而同时承担了：

- CPU/陷阱/IRQ/tick；
- 控制台 lower-half；
- Flash/块设备；
- 任务保护和内存窗口；
- GPIO/ADC/PWM/UART/SPI/I2C/DMA/PIO；
- 启动诊断、cache、USB poll、UART probe 等板级运维能力。

### 3.3 测试和 CI 现状

CI 已包含：

- Go test/vet/goc32/rv32emu：[images.yml L18-L39](file:///d:/projects/TinyOS/.github/workflows/images.yml#L18-L39)
- 多目标 flashable image 构建：[images.yml L42-L188](file:///d:/projects/TinyOS/.github/workflows/images.yml#L42-L188)
- 宿主工具构建：[images.yml L190-L266](file:///d:/projects/TinyOS/.github/workflows/images.yml#L190-L266)
- QEMU smoke：[images.yml L271-L320](file:///d:/projects/TinyOS/.github/workflows/images.yml#L271-L320)
- release job 依赖 checks/build/smoke/tools：[images.yml L322-L338](file:///d:/projects/TinyOS/.github/workflows/images.yml#L322-L338)

本地本轮验证：

```text
go test ./...  通过
go vet ./...   通过
```

[TESTING.md](file:///d:/projects/TinyOS/docs/TESTING.md#L15-L24) 已定义 T0-T4 测试级别，工程纪律较强。剩余短板不是“没有 CI”，而是 C 内核纯逻辑缺少宿主侧快速测试层。

---

## 4. 架构问题详述

## P0-1：平台抽象层过宽，依赖方向没有被强制约束

### 证据

[platform.h](file:///d:/projects/TinyOS/kernel/platform.h#L7-L12) 的注释说明内核只能调用平台函数、不能直接碰架构状态，这个原则是正确的；但同一个头文件继续混合了多类接口：

- 控制台 lower-half：[L55-L100](file:///d:/projects/TinyOS/kernel/platform.h#L55-L100)
- timer/IRQ/CPU：[L102-L155](file:///d:/projects/TinyOS/kernel/platform.h#L102-L155)
- 块设备/Flash：[L252-L290](file:///d:/projects/TinyOS/kernel/platform.h#L252-L290)
- GPIO/ADC/PWM/UART/PIO/SPI/I2C/DMA：[L350-L425](file:///d:/projects/TinyOS/kernel/platform.h#L350-L425)
- 用户态保护段：[L427-L494](file:///d:/projects/TinyOS/kernel/platform.h#L427-L494)

板级实现规模也偏大：RP2350 ARM/RV 两个 `platform.c` 都超过 2,000 行，RP2040 约 1,435 行。

### 问题

当前目录上虽然区分了 `arch/`、`board/`、`drivers/`，但没有编译期规则禁止：

- 驱动直接 include 内核私有头；
- 板级代码承担通用驱动状态机；
- 一个新外设继续把接口加进全局 `platform.h`；
- 上层模块绕过 `devfs/hwdev` 直接调用具体板级函数。

这会使“平台层”逐渐变成所有未归类代码的集合点。

### 成熟项目对照

- **Zephyr**：device model 将设备实例、config/data、driver API、HAL 和 Devicetree 资源分离；驱动通过类 API 暴露能力。
- **NuttX**：upper-half driver 放通用协议/缓冲/阻塞语义，lower-half driver 只做硬件动作；VFS/file_operations 统一入口。
- **FreeRTOS**：portable 层保持窄接口，BSP 和驱动不通过一个巨型 `port.h` 暴露所有外设。

### 建议

第一阶段不改变运行时行为，只拆头文件和依赖：

```text
include/plat/irq.h
include/plat/time.h
include/plat/console.h
include/plat/blockdev.h
include/plat/flash.h
include/plat/memprot.h
include/plat/gpio.h
include/plat/i2c.h
include/plat/spi.h
include/plat/dma.h
include/plat/diag.h
```

同时增加架构约束测试：

- `kernel/` 不允许 include 具体板级头；
- `drivers/` 不允许 include `process.h`、`ramdisk.h` 等内核私有头，除非白名单；
- `board/` 不实现通用协议状态机；
- 新增 `plat_*` 必须归类到窄头文件，不能继续追加到单一 `platform.h`。

### 触发条件

当新增板卡、第二块 Flash/TF 卡、新总线或新保护后端时，如果不先收口，复制和耦合会继续放大。

---

## P0-2：`process.c` 仍是内核巨型模块，职责边界不足

### 证据

[process.c](file:///d:/projects/TinyOS/kernel/process.c) 约 3,784 行，当前同时包含：

- 固定进程表和 `proc_t`：[L24-L35](file:///d:/projects/TinyOS/kernel/process.c#L24-L35)
- 任务状态枚举：[L111-L134](file:///d:/projects/TinyOS/kernel/process.c#L111-L134)
- FD 类型和 `file_t`：[L136-L163](file:///d:/projects/TinyOS/kernel/process.c#L136-L163)
- 进程结构和窗口字段：[L171-L210](file:///d:/projects/TinyOS/kernel/process.c#L171-L210)
- 状态机合法性和 `proc_set_state()`：[L280-L322](file:///d:/projects/TinyOS/kernel/process.c#L280-L322)
- per-CPU 调度状态和 `proc_lock`：[L360-L394](file:///d:/projects/TinyOS/kernel/process.c#L360-L394)
- checked add/align：[L445-L474](file:///d:/projects/TinyOS/kernel/process.c#L445-L474)
- exec format 注册表和加载路径仍位于同一翻译单元。

### 问题

`exec_format` 已经把具体 LZ4TOS/ELF handler 从直接调用变成表驱动，这是正确方向；但注册表、进程生命周期、FD、窗口分配和调度仍在同一个文件中。结果是：

1. 修改调度器时容易碰到 exec/FD/线程逻辑。
2. 拆锁或引入新等待对象时，很难只验证一个子系统。
3. host-native 单元测试无法只链接调度器或 FD 表。
4. SMP、kernel thread、thread join、signal、exec 并发继续增加状态组合。

### 成熟项目对照

- **xv6**：即使内核很小，也拆成 `proc.c`、`fs.c`、`vm.c`、`sysfile.c`、`pipe.c` 等模块。
- **NuttX**：`sched/`、`task/`、`fs/inode`、`binfmt/`、`mm/` 分离。
- **Zephyr**：scheduler、IPC、memory domain、device、userspace 分属不同子系统。

### 建议拆分路线

保持 ABI、syscall 编号和正常路径语义不变：

| 新模块 | 从 `process.c` 迁出的职责 |
|---|---|
| `sched.c` | runqueue、tick、sleep/wake、调度选择、slice、per-CPU current |
| `task.c` | `proc_t` 生命周期、exit/wait、signal、thread group |
| `fd.c` | `file_t`、fd table、dup/dup2/clone/close |
| `uaccess.c` | `process_user_ok`、用户字符串和缓冲区复制 |
| `window.c` | 窗口几何、win_alloc/free、sbrk、stack grow |
| `exec_registry.c` | exec container/image handler 注册与 probe 分发 |
| `ktask.c` | kernel thread stack、create/exit/join |

拆分顺序建议：`uaccess` → `fd` → `exec_registry` → `window` → `sched/task`。每一步都用现有 QEMU smoke 和 soak 做行为等价验证。

进度：`uaccess` 已在 2026-11-22 抽出（`kernel/uaccess.c`，卡
`2026-11-22-uaccess-extract-d0cfe8d-qemu.md`）；`fd` 在 2026-11-26 抽出
（`kernel/file.h` + `kernel/fd_internal.h` + `kernel/fd.c`，卡
`2026-11-26-fd-module-e83b517.md`），read/write 与管道阻塞留在 `process.c`，用 5 个
hook 反向调用。下一个按路线是 `exec_registry`。

---

## P1-1：WiFi 协议栈与芯片驱动、设备节点耦合过深

### 证据

[wifi_net.c](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_net.c#L1-L18) 文件头直接说明它实现了最小 IPv4 栈：ARP、IPv4、ICMP、DHCP，并通过 `/dev/net` 暴露命令式接口。

全局状态 `ns` 同时包含：

- IP/mask/gw/dns/lease：[L111-L126](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_net.c#L111-L126)
- ARP、ping、DNS、TCP/HTTP 状态：[L127-L170](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_net.c#L127-L170)
- 4 KiB TCP RX buffer 和 HTTP request buffer：[L159-L166](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_net.c#L159-L166)
- 后台 `net_task`：[L1268-L1439](file:///d:/projects/TinyOS/drivers/wifi/esp32c3/wifi_net.c#L1268-L1439)

### 问题

当前实现对 ESP32-C3 单 station、单 TCP 辅助 HTTP 场景是合理的 footprint 取舍，但架构上存在以下限制：

1. 协议栈位于 `drivers/wifi/...`，无法复用到以太网或第二块无线芯片。
2. `/dev/net` 是文本命令接口，不是标准流式 FD/socket 模型。
3. 全局单例 `ns` 难以支持多接口、多 TCP 连接或多个并发 client。
4. DHCP renew、DNS、ping、HTTP、TCP、ARP 在同一状态机里，后续改动容易相互影响。
5. 驱动和协议测试只能走端到端 QEMU/硬件，难以对 DHCP/TCP 状态机做宿主单测。

### 成熟项目对照

- **Zephyr**：`net_if` 管理网络接口，offloaded device 可把 WiFi 芯片能力接入统一网络栈，socket API 与 FD/事件模型集成。
- **NuttX**：lwIP 通过 socket/VFS 暴露，网卡 lower-half 只负责 L2 收发。
- **FreeRTOS+TCP**：网络接口层把不同 MAC/WiFi 驱动接入同一 IP/TCP 栈。

### 建议

不建议立刻引入完整 lwIP。当前文档已说明 XIP 窗口和 footprint 不允许这么做。建议先建立轻量 seam：

```text
drivers/wifi/esp32c3/
  只保留 init/scan/connect、L2 tx/rx、事件回调

net/
  net_if.c        接口注册、MAC/MTU/link state
  arp.c
  ipv4.c
  icmp.c
  dhcp.c
  dns.c
  tcp.c
  net_dev.c       /dev/net 或未来 socket FD 的适配层
```

第一阶段只移动代码和拆状态，不改变 `/dev/net` 命令；第二阶段再把连接对象映射为 FD，使 `read/write/poll/close` 可以复用。

### 明确不做

- 不为了“架构完整”立即引入 lwIP。
- 不实现全 POSIX socket。
- 不在没有第二网卡需求时做多接口动态管理。

触发条件：第二块网卡、以太网、多 TCP 连接、server 场景或协议状态机继续扩大。

---

## P1-2：文件系统与块设备仍是 littlefs/ramdisk 直连，VFS/MTD seam 尚未建立

### 证据

[ramdisk.c](file:///d:/projects/TinyOS/kernel/ramdisk.c#L42-L51) 持有全局 littlefs、lfs_config、read/prog/lookahead buffer 和 4 KiB bounce buffer；[L76-L105](file:///d:/projects/TinyOS/kernel/ramdisk.c#L76-L105) 说明整个 API 调用由一个 `fs_lock` 保护，并管理打开文件表。

块 I/O 直接调用 `plat_block_read/write`：[L126-L177](file:///d:/projects/TinyOS/kernel/ramdisk.c#L126-L177)、[L179-L209](file:///d:/projects/TinyOS/kernel/ramdisk.c#L179-L209)。

### 问题

当前只有 littlefs 时，这种设计简单、锁顺序清楚、性能也有优化依据。但它把以下概念绑定在一起：

- 文件系统类型；
- 块/Flash 设备；
- 打开文件表；
- 分区常量；
- readdir 快照；
- bounce/cache 策略；
- syscall 可调用的文件操作。

一旦加入 FAT、ROMFS、TF 卡、PSRAM 块设备或多分区，`syscall.c`、`process.c` FD 逻辑和 `ramdisk.c` 会被迫一起改。

### 成熟项目对照

- **NuttX**：VFS/inode 统一普通文件、设备、socket 和挂载点；MTD 层描述 Flash 几何、擦除、XIP 能力和分区。
- **Zephyr**：FS API 背后可接 littlefs/FATFS，底层通过 flash/disk 接口隔离硬件。

### 建议

先做轻量 FS seam，不做完整 VFS：

```c
struct fs_backend {
    const char *name;
    int (*open)(...);
    int (*read)(...);
    int (*write)(...);
    int (*close)(...);
    int (*stat)(...);
    int (*readdir)(...);
    int (*unlink)(...);
};

struct block_device {
    int (*read)(uint32_t lba, uint32_t count, void *buf);
    int (*write)(uint32_t lba, uint32_t count, const void *buf);
    int (*erase)(uint32_t lba, uint32_t count);
    uint32_t (*block_size)(void);
    uint32_t (*block_count)(void);
};
```

短期仍只注册一个 littlefs backend，但 syscall/FD 不再直接知道 littlefs 细节。

### 触发条件

- 第二文件系统；
- TF 卡/外部 Flash；
- PSRAM 块设备；
- 多分区挂载；
- 需要 XIP metadata 或可升级分区布局。

在这些需求出现前，不建议照搬 NuttX VFS。

---

## P1-3：内存域接缝已建立，但仍是单连续可执行窗口模型

### 证据

[mem_domain.h](file:///d:/projects/TinyOS/kernel/mem_domain.h#L6-L13) 明确说明当前只有一个 executable domain，结构体存在是为了未来追加 data-only domain；[mem_domain.c](file:///d:/projects/TinyOS/kernel/mem_domain.c#L4-L17) 只初始化一个 `app_domain`，其 base/size/grain 来自 `PLAT_APP_*`。

[platform.h](file:///d:/projects/TinyOS/kernel/platform.h#L429-L437) 也明确当前没有地址空间抽象，内核直接访问用户内存，并依靠 `process_user_ok` 和 PMP/MPU 限制用户态。

### 问题

单域模型对当前 SRAM 任务窗口非常高效，但未来 PSRAM、第二 heap、非连续内存或数据池会遇到：

- 可执行内存和数据内存需要不同权限；
- 窗口分配不能只扫描一个连续区间；
- PMP/MPU/none 后端需要统一的 domain apply 接口；
- cache/DMA 一致性和内存属性可能不同；
- `win_alloc` 的 domain flags 需要真正参与分配策略。

### 成熟项目对照

- **Zephyr memory domain**：线程加入内存域，架构后端负责把内存分区编程到 MPU/MMU。
- **NuttX protected/kernel build**：flat、protected、kernel 模式明确区分无 MMU、MPU 保护和完整地址空间。

### 建议

保持当前单域行为不变，先补齐后端接口和 host fake：

```text
mem_domain
  alloc_window(domain, bytes, attrs)
  free_window(domain, window)
  apply_for_current(thread, segments)
  validate_user_range(thread, addr, len, access)
  stats(domain)
```

在 PSRAM 真正接入前，用 fake domain 做单元测试，验证 data-only domain 不可执行、分配失败和跨域拒绝。

### 明确不做

- 不提前引入页表；
- 不把 PSRAM 做成可执行窗口；
- 不改变现有 PIE 加载和单域正常路径。

---

## P1-4：设备模型有静态表和类钩子，但缺统一生命周期、依赖和电源状态

### 证据

[devfs.h](file:///d:/projects/TinyOS/kernel/devfs.h#L21-L45) 的 `dev_ops` 已包含 open/read/write/ioctl/close；[L62-L74](file:///d:/projects/TinyOS/kernel/devfs.h#L62-L74) 规定设备在 boot 时注册，之后表只读。

[hwdev.h](file:///d:/projects/TinyOS/kernel/hwdev.h#L7-L27) 则通过 board device table 和 weak class hooks 管理 LED、key、display 等设备。

### 问题

这套模型足够小，也避免了设备框架过重，但缺少：

- init level 和依赖顺序；
- `deinit`/hotplug；
- `suspend/resume/runtime PM`；
- 设备状态机；
- 驱动资源占用和释放的统一报告；
- 多实例设备的统一生命周期。

目前主要靠 `kmain()` 启动顺序和板级代码约定保证正确。设备少时问题不明显，USB、LCD、Flash、WiFi、DMA、console 互相依赖后会变脆弱。

### 成熟项目对照

- **Zephyr**：设备有 init level、依赖、config/data 分离、runtime PM 和 Devicetree 实例化。
- **NuttX**：driver lower-half 有注册/拆除和连接状态语义。

### 建议

不引入完整 Zephyr device model，先定义轻量设备注册表：

```c
struct device {
    const char *name;
    enum device_class class;
    int init_level;
    int (*init)(void);
    int (*deinit)(void);
    int (*suspend)(void);
    int (*resume)(void);
    const void *ops;
    const char *const *deps;
};
```

先用于 USB、LCD、WiFi、Flash 这类有明显依赖和电源语义的设备；GPIO 这类简单设备继续保留现有 weak hooks。

---

## P2-1：SMP 锁粒度和实时调度能力仍偏初级

### 证据

[process.c](file:///d:/projects/TinyOS/kernel/process.c#L377-L394) 明确说明进程表使用一把 `proc_lock`，临界区主要是小表扫描和状态存储；[ramdisk.c](file:///d:/projects/TinyOS/kernel/ramdisk.c#L76-L103) 使用一把 `fs_lock` 包住整个 littlefs API 调用。

当前优先级只有 3 档：[process.c L100-L109](file:///d:/projects/TinyOS/kernel/process.c#L100-L109)，时间片为 1 tick：[L74-L98](file:///d:/projects/TinyOS/kernel/process.c#L74-L98)。

### 评价

全局锁在当前表大小和短临界区下是合理选择，不应为了“更高级”而过早拆锁。真正的风险是未来如果出现以下场景：

- 长持有 `fs_lock` 的整文件操作；
- 驱动在锁内等待硬件；
- 多 CPU 高频并发 exec；
- 优先级反转影响实时任务；
- 网络/USB workqueue 引入更多阻塞路径。

### 成熟项目对照

- FreeRTOS SMP 支持核心亲和性、调度锁和 SMP 相关配置。
- Zephyr 提供 spinlock、mutex、workqueue、message queue，以及优先级继承互斥锁。
- NuttX 文档和本地对照笔记都强调 `spin_lock_irqsave` 这种命名纪律。

### 建议

1. 先画锁顺序图：`proc_lock`、`fs_lock`、`heap_lock`、pipe lock、driver lock。
2. 增加静态测试或文档检查，禁止在 `proc_lock/fs_lock` 内调用可能阻塞的路径。
3. 把 IRQ save + spin lock 封装成命名清晰的 helper，减少误用。
4. 只有在实测竞争或长临界区出现后再拆 FD 表、runqueue、文件系统锁。
5. PI mutex、5 档优先级、affinity、tickless 继续保持条件项，不提前实现。

---

## P2-2：Go 构建器已分层，但 `Builder` 仍然过大

### 证据

[build.go](file:///d:/projects/TinyOS/internal/build/build.go) 约 1,756 行，包含 30 个函数/类型，覆盖：

- target/window note；
- Builder 状态；
- 工具调用；
- C flags；
- 编译/链接；
- task 并行；
- 总 Build 编排；
- boot/kernel/user/fs/disk image；
- ramdisk blob。

好的一面是，目标、板级、chip 配置已经拆到 [targets.go](file:///d:/projects/TinyOS/internal/build/targets.go)、[boardconf.go](file:///d:/projects/TinyOS/internal/build/boardconf.go)、[chip.go](file:///d:/projects/TinyOS/internal/build/chip.go)，`internal/build` 下已有 41 个 Go 文件和较多测试。

### 建议拆分

```text
builder.go        只保留 Build() 编排和任务依赖
compiler.go       ccFlags/compile/toolchain
linker.go         link/linkPIE/ld flags
userbuild.go      用户程序收集、裁剪、构建
fsbuild.go        mklfs/littlefs/ramdisk blob
imagebuild.go     disk/UF2/ESP image
windownote.go     ELF note 和窗口 hint
```

这项是可维护性重构，不应改变任何产物字节；适合配合 golden artifact 测试推进。

---

## P2-3：ELF/LZ4/window 规则存在多语言重复，需要共享 corpus

### 证据

同一类格式规则分布在：

- 内核：[elf.c](file:///d:/projects/TinyOS/kernel/elf.c)、[lz4.c](file:///d:/projects/TinyOS/kernel/lz4.c)
- 宿主 C 打包工具：[mklfs.c](file:///d:/projects/TinyOS/tools/mklfs.c)
- Go 构建器：[build.go](file:///d:/projects/TinyOS/internal/build/build.go)
- goc32：`goc32/internal/lz4tos`
- 测试：`windownote_test.go`、`lz4hint_test.go`、`corrupt_test.go`、`execformat_test.go`

ABI 编号和结构体已经通过 [syscalls.csv](file:///d:/projects/TinyOS/abi/syscalls.csv)、[tos_abi.h](file:///d:/projects/TinyOS/include/tos_abi.h) 和快照测试降低漂移；但 ELF note、窗口几何、LZ4TOS header、畸形输入容错仍有多份解析逻辑。

### 风险

- C 修复了某个畸形输入，Go/goc32 仍可崩溃或接受错误输入。
- 窗口 hint 规则变更后，打包器和内核理解不一致。
- 正常 ELF 测试覆盖充分，但畸形输入语料分散。

### 建议

建立共享 corpus，而不是强行用一种语言重写所有解析器：

```text
tests/corpus/elf/
  valid/
  malformed-phdr/
  malformed-note/
  overflow-window/
  bad-reloc/
tests/corpus/lz4tos/
  v0/
  v1/
  truncated/
  bad-magic/
  oversized-raw/
```

每个实现都消费同一批输入，并输出统一结果码：accept / reject / panic-forbidden。Go 可用 table-driven test，C 可通过 host test 或 QEMU mock 加载。

---

## P2-4：缺少 host-native C 内核单元测试层

### 证据

当前 T0 主要是 Go 测试，T2 是 QEMU smoke/soak，T3 是真机，T4 是压力测试，见 [TESTING.md L15-L24](file:///d:/projects/TinyOS/docs/TESTING.md#L15-L24)。

C 内核中的以下纯逻辑很适合宿主测试，但目前主要通过模拟器端到端覆盖：

- `process_user_ok` 和用户字符串复制；
- 窗口几何、checked add/align；
- ELF program header/note/relocation 解析；
- LZ4 流式解压边界；
- pipe ring 和引用计数；
- mailbox；
- 调度状态机；
- littlefs block 地址换算。

### 成熟项目对照

- Zephyr 使用 native_sim 和 twister，在宿主上快速跑大量内核/驱动行为测试。
- NuttX 有 sim 目标。
- 教学/小型内核通常也会把纯算法和状态机从硬件依赖中抽出后在宿主测试。

### 建议

新增最小 host test，不追求模拟全部硬件：

```text
tests/host/
  stubs/
    plat_irq.c
    plat_console.c
    plat_timer.c
    plat_memprot.c
    plat_block.c
  test_uaccess.c
  test_window.c
  test_elf.c
  test_lz4.c
  test_pipe.c
  test_sched_state.c
```

原则：

- host test 只测纯逻辑和显式 stub 后的状态机；
- 不替代 QEMU/真机，只承担快速边界回归；
- 畸形输入和整数溢出优先进入 host test；
- CI T0 增加 host test 构建和执行。

---

## P2-5：配置体系已显著改善，但仍需防止“静默默认值”回流

### 证据

[targets.go](file:///d:/projects/TinyOS/internal/build/targets.go#L111-L159) 对 target 的整数和 size 字段已有显式错误；[boardconf.go](file:///d:/projects/TinyOS/internal/build/boardconf.go#L66-L92) 会校验 schema、chip 和必填字段；Flash 几何也有范围检查：[L170-L204](file:///d:/projects/TinyOS/internal/build/boardconf.go#L170-L204)。

这说明早期“非法值静默回默认”的问题已经大幅改善。

### 剩余风险

- 配置 key 拼写错误仍可能表现为“未设置/默认值”，需要依赖未知 key 审计。
- 能力 feature 与板级 peripherals 的映射若不全，可能出现配置声明了但硬件不支持的功能。
- 多层配置叠加后，最终生效值需要更容易追踪。

### 建议

1. INI parser 增加 unknown key/unknown section 检查，至少在 strict 模式报错。
2. `build-config.txt` 同时记录“来源文件 + 最终值 + 默认值/覆盖值”。
3. 对所有 feature 建立 capability 反查，不支持则构建期失败。
4. 配置测试继续表驱动化，覆盖负数、未知单位、溢出、重复 key 和冲突配置。

---

## 5. 已经做得好、不应误判为问题的设计

### 5.1 不引入完整 Kconfig/Devicetree 是合理取舍

[ARCHITECTURE-NEXT.md L433-L439](file:///d:/projects/TinyOS/docs/ARCHITECTURE-NEXT.md#L433-L439) 明确不做 Kconfig/CMake/TOML/外部代码生成器。对 TinyOS 当前 18 个目标和单一 Go 构建器而言，INI + 强校验 + 生成物的 footprint 和理解成本更低。

建议是继续强化 schema 和约束，而不是照搬 Zephyr 的完整配置体系。

### 5.2 暂不做完整 VFS/POSIX 是合理取舍

本地 [NUTTX-COMPARE.md](file:///d:/projects/TinyOS/docs/NUTTX-COMPARE.md#L183-L187) 已说明 NuttX VFS 的规模和 TinyOS 当前 devfs 体量差异。当前没有第二文件系统时，完整 VFS 会成为最大内核模块之一。

合理路线是先留 FS seam，等第二后端出现再实现。

### 5.3 x86/RP2040 无 MMU/MPU 不应作为新漏洞

无分页或无 MPU 是硬件/项目既定限制。TinyOS 的任务窗口、用户指针校验和可检测故障已经优于完全 flat 的裸机程序。报告不应把“没有完整地址空间”当成缺陷，只能作为架构能力边界。

### 5.4 单 littlefs 全局锁当前合理

[ramdisk.c](file:///d:/projects/TinyOS/kernel/ramdisk.c#L76-L103) 对为什么锁粒度是整个 API 调用有充分解释：littlefs cache、bounce、handle table 和 readdir snapshot 都是进程无关全局状态。没有实测竞争前不应盲目拆锁。

### 5.5 exec format 和 ABI 契约方向正确

[exec_format.h](file:///d:/projects/TinyOS/kernel/exec_format.h#L8-L25) 已把 container format 和 image format 分离，并要求 handler 声明版本；[tos_abi.h](file:///d:/projects/TinyOS/include/tos_abi.h#L4-L19) 明确 ABI 结构只追加、不重排。这是后续可扩展加载器的基础。

---

## 6. 成熟项目对照总表

| 维度 | TinyOS 当前方式 | Zephyr | NuttX | FreeRTOS / xv6 / Minix | TinyOS 建议 |
|---|---|---|---|---|---|
| 移植层 | 宽 `platform.h`，78 个 `plat_*` 声明 | driver API + HAL + DTS | arch/board/driver 分层，upper/lower-half | FreeRTOS 窄 portable | 拆窄头文件，加 include 约束 |
| 设备模型 | `devfs` 静态表 + `hwdev` class hooks | device model、init level、PM | file_operations、driver register | xv6 简单设备表；Minix 用户态驱动 | 先加轻量 device registry 和生命周期 |
| 调度 | 3 优先级、RR、per-CPU runqueue、全局 proc_lock | 多优先级、SMP、workqueue、PM | sched/task 分离、tickless 可选 | FreeRTOS SMP affinity | 先拆模块和锁纪律，高级特性按需求启用 |
| 内存 | 单任务窗口、PMP/MPU/none、单 mem domain | memory domain、userspace、MMU/MPU | flat/protected/kernel | xv6 VM；Minix 用户态隔离 | 保留单域，完善 domain backend 和 fake test |
| 文件系统 | littlefs + ramdisk 直连 | FS API + flash/disk | VFS/inode + MTD | xv6 fs/inode | 第二后端出现前先做 FS/block seam |
| 网络 | ESP32-C3 自研最小 IPv4/TCP，`/dev/net` 命令 | net_if + socket/offload | lwIP + socket/VFS | FreeRTOS+TCP | 抽 `net_if`，协议栈移出驱动目录 |
| 程序加载 | LZ4TOS/ELF handler 表、PIE、窗口加载 | LLEXT | binfmt | xv6 ELF；Minix 文件服务 | 保持 handler 表，补共享畸形 corpus |
| 配置 | Go + INI + board/chip/target 三层 | Kconfig + Devicetree | Kconfig/defconfig | 各项目自定义 | 不照搬，强化 schema/unknown key/来源追踪 |
| 测试 | Go test、QEMU、真机、soak、rv32emu/goc32 | native_sim/twister | sim | xv6 lab tests | 增 host-native C 单元测试 |
| 驱动隔离 | 内核态驱动 | 内核/用户态可选 | 内核/protected | Minix 用户态驱动 | 当前不采用用户态驱动，footprint/复杂度不匹配 |

参考资料：

- Zephyr Driver Model: <https://docs.zephyrproject.org/latest/kernel/drivers/index.html>
- Zephyr Devicetree: <https://docs.zephyrproject.org/latest/build/dts/index.html>
- Zephyr Memory Domains: <https://docs.zephyrproject.org/latest/kernel/services/memory_domain.html>
- NuttX Documentation: <https://nuttx.apache.org/docs/latest/>
- FreeRTOS SMP: <https://www.freertos.org/Documentation/02-Kernel/03-Supported-kernel-features/07-SMP-symmetric-multiprocessing>
- MINIX 3: <https://minix3.org/>

---

## 7. 建议路线图

## 阶段 0：基线与护栏（1 个小周期，不改行为）

目标：先让后续拆模块有安全网。

1. 固化当前 `go test ./...`、`go vet ./...`、QEMU smoke/soak 基线。
2. 增加 include 依赖检查脚本或 Go 测试：
   - drivers 不得 include 内核私有头；
   - kernel 不得 include 板级私有头；
   - 新 `plat_*` 必须进入窄接口。
3. 建立 ELF/LZ4TOS 畸形输入 corpus 目录。
4. 为 `process_user_ok`、checked add/align、ELF note 解析准备 host test stub。

验收：全部现有目标行为不变，测试结果可复现。

## 阶段 1：窄化平台接口（P0）

1. 从 [platform.h](file:///d:/projects/TinyOS/kernel/platform.h) 拆出 console、irq/time、block/flash、memprot、peripheral 等头。
2. 保留一个兼容 `platform.h` 聚合 include，逐步迁移调用点。
3. 把 RP2350/RP2040 `platform.c` 中通用总线/驱动逻辑迁到 `drivers/<class>` 或 board support 文件。
4. 新增接口必须说明上下文：ISR、tick、task、是否允许阻塞、是否持锁。

验收：每个目标编译和 smoke 不变；`platform.h` 不再新增外设接口。

## 阶段 2：拆分 `process.c`（P0）

推荐顺序：

1. `uaccess.c`：用户指针和字符串复制。
2. `fd.c`：FD 表和 clone/close。
3. `exec_registry.c`：handler 注册和 probe。
4. `window.c`：窗口几何、分配、sbrk、stack grow。
5. `sched.c` / `task.c`：调度和进程生命周期分离。
6. `ktask.c`：内核线程。

验收：ABI 不变；QEMU rv32/rv32smp/arm smoke + soak 通过；SMP exec/piperace/dd stress 不回退。

## 阶段 3：网络和存储 seam（P1）

1. 将 `wifi_net.c` 的协议逻辑移到 `net/`，保留 `/dev/net` 命令兼容。
2. 定义 `net_if` 和 L2 send/receive/link state。
3. 定义 `block_device` 和 `fs_backend`，littlefs 成为第一个 backend。
4. 不改用户可见命令和文件路径。

验收：现有 ping/http/dhcp 行为不变；第二后端只需实现接口，不改 syscall。

## 阶段 4：设备生命周期与多域预留（P1/P2）

1. 为 USB、LCD、WiFi、Flash 建立轻量 device registry。
2. 增加 `suspend/resume` 状态，但先不做全系统电源管理。
3. 用 fake backend 测试 data-only mem domain。
4. PSRAM 落地时只先做数据池，不做可执行窗口。

## 阶段 5：条件性高级能力

以下能力不预先排期，由真实需求触发：

- socket FD / lwIP；
- 完整 VFS/MTD；
- tickless/低功耗；
- 优先级继承/affinity/更多优先级；
- workqueue；
- 用户态驱动；
- 完整 MMU/分页。

---

## 8. 风险与取舍

| 行动 | 收益 | 主要风险 | 缓解 |
|---|---|---|---|
| 拆 `platform.h` | 依赖清晰、新板不再堆上帝接口 | 改动面广 | 先保留聚合头，逐模块迁移 |
| 拆 `process.c` | 降低核心回归风险，利于 host test | 调度/锁语义容易被破坏 | 小步提交，每步跑 rv32smp soak |
| 抽 `net_if` | 协议栈可复用，支持第二网卡 | 当前 `/dev/net` 行为可能变化 | 第一阶段只搬代码不改接口 |
| 抽 FS backend | 支持第二存储/文件系统 | 过度设计 | 只做 seam，不实现完整 VFS |
| host C test | 快速覆盖边界和状态机 | 需要维护 stub | 只抽纯逻辑，硬件仍由 QEMU/真机测 |
| 引入完整 RTOS 框架 | 功能多 | footprint、学习成本、失控抽象 | 当前明确不采用 |

---

## 9. 结论与建议决策

### 9.1 建议立即批准的事项

1. **平台接口窄化立项**：这是所有后续板级和驱动扩展的前提。
2. **`process.c` 拆分设计**：先出无行为变化的模块迁移计划，不直接改调度策略。
3. **host-native C 测试最小试点**：从 `uaccess/window/elf note` 三个纯逻辑点开始。
4. **ELF/LZ4TOS corpus 建设**：把畸形输入测试从分散单测收敛为共享语料。

### 9.2 建议继续观察、暂不实现的事项

1. 完整 VFS/POSIX。
2. 完整 Kconfig/Devicetree。
3. lwIP 或完整 socket。
4. tickless/电源管理。
5. 优先级继承、affinity、5 档优先级。
6. 用户态驱动。
7. PSRAM 可执行窗口。

### 9.3 一句话结论

TinyOS 的下一阶段重点不应是“再支持一块板”或“再加一个协议”，而应是 **把现有 18 个目标、SMP、用户态窗口、动态加载、WiFi、Flash 和测试体系背后的接缝收窄、固化并可测试化**。只要保持“先 seam、后替换；先测试、后迁移；正常路径 ABI 不变”的原则，就能在不引入 Zephyr/NuttX 级别复杂度的情况下，显著提高后续演进的安全性和可维护性。
