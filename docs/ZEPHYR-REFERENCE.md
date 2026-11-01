# Zephyr 参考源码笔记

Zephyr 不是依赖，只是参考资料：它实现了与我们同类的东西（RISC-V ELF 动态加载、
PMP、ESP32-C3 板级支持），其中 relocation 解析是全网最完整的开源实现。
本文件记录"我们参考了什么、学到了什么、为什么不照抄"。

## 获取方式（可复现）

完整仓库太大（含全部 boards/tests），用稀疏检出只取需要的部分：

```sh
git -c core.longpaths=true -c core.autocrlf=input clone --depth=1 \
    --filter=blob:none --sparse https://gitee.com/mirrors/zephyr.git \
    third_party/zephyr
cd third_party/zephyr
git -c core.longpaths=true sparse-checkout set \
    subsys/llext arch/riscv include/zephyr/llext doc/services/llext
git -c core.longpaths=true sparse-checkout add \
    boards/espressif soc/espressif dts/riscv/espressif
```

- gitee.com/mirrors/zephyr 是 GitHub 的官方同步镜像，国内直连约 5 秒
  （github.com 本体经代理 502，raw.githubusercontent.com 连不上）。
- 检出约 15 MB；`/third_party/zephyr/` 已列入 .gitignore，永不入库。
- 版本：4.5.0-rc3（2026 年）。

## 学到的东西

### 1. RISC-V relocation 的完整参考实现

`arch/riscv/core/elf.c`（534 行）是 RISC-V psABI 全套重定位的开源实现，
我们的 PIE 加载器可以逐条对照。三个要点：

**a) PCREL_HI20 / PCREL_LO12 配对。** LO12 重定位的"符号"不是真实符号——
它的 st_value 是同 section 内那条 HI20 指令的偏移。解析顺序：

```
遇到 LO12 → 扫描本 section 的重定位表找到 r_offset == st_value 的 HI20
         → 从 HI20 条目取真实符号 → S + A - P
         → 高 20 位填进 HI20 指令，低 12 位填进 LO12 指令
```

Zephyr 用线性扫描 + 环形回绕 + 一个 `riscv_last_rel_idx` 游标缓存上次命中位置
（"HI20 大概率按序出现"）。注意这个游标是**静态全局变量且无锁**——并发加载
会互相污染，我们的实现要么不用游标、要么把它挂到加载上下文里。

**b) HI20 写入的 sign-fixup。** `imm8 += imm8 & 0x800`：立即数 bit12 为 1 时
LO12 按负数解码，会把 HI20 的值借走 1，所以 HI20 要预先加 1。这是 `la`
伪指令的标准行为，漏写就是"偶然对偶然错"。

**c) Zephyr 的范围检查有两处错误，不要抄：**

| 重定位 | Zephyr 检查上限 | 正确上限 |
|---|---|---|
| `R_RISCV_JAL` | INT32_MAX（U+I 对的） | ±1 MiB（J 型 21 位立即数） |
| `R_RISCV_LO12_I` | INT32_MAX | ±2 KiB（I 型 12 位立即数） |

JAL 超过 1 MiB 时 Zephyr 会静默截断高位、跳到错误地址。我们的实现按编码
位宽逐类型检查（B ±4 KiB、CB ±256 B、CJ ±2 KiB、S/I ±2 KiB）。

**d) `R_RISCV_ALIGN` 直接拒绝（-ENOEXEC）。** 它要求移动后续所有符号，
动态加载器做不到。这印证了探针阶段的结论：加载用 ELF 必须在链接时
`--no-relax`（否则 -mcmodel=medany 会产生 R_RISCV_ALIGN 和 relax 序列）。
`R_RISCV_RELAX` 则可以忽略——它只是链接期优化提示。

### 2. ESP32-C3 内存常量（修正了我们记错的值）

`soc/espressif/esp32c3/memory.h` + `dts/riscv/espressif/esp32c3/esp32c3_common.dtsi`：

```
SRAM0  0x4037C000  16 KiB    ICache，不可当普通 RAM
SRAM1  0x3FC80000  384 KiB   DRAM；IRAM 别名 = +0x700000 = 0x40380000
RTC_FAST 0x50000000 8 KiB
INTMTX 0x600C2000            （与探针实测一致）
```

ROM 占用（DRAM 视角，simple boot 无二级 bootloader 时）：

```
0x3FCCAE00  ROM 共享缓冲区起点（下载模式用）
0x3FCDC710  ROM 栈区结束 = 用户可用上界  ★
0x3FCDE710  ROM .bss/.data 起点（不可回收）
```

所以用户可用 SRAM = `0x3FC80000 .. 0x3FCDC710` = **369.8 KiB**（IRAM 视角
`0x40380000 .. 0x403DC710`）。此前记忆里的上界 0x3FCCE710 是错的，已修正。

### 3. C3 的 flash XIP 窗口

```
IROM  0x42000000  取指映射（flash 代码）
DROM  0x3C000000  数据映射（flash 只读）
```

**原先的推断是错的，已实测否定。** 当时的理由是"ROM 自身 XIP 执行，所以窗口
在跳 entry 前就配好了"——但 C3 的 ROM 是掩膜 ROM（0x40000000），它不需要 flash
窗口；flash 的 cache/MMU 是**二级引导**建立的，而我们绕过了二级引导（镜像直接
由 ROM 装进 SRAM）。`scripts/esp-c3/drom.S` 在真机上读 0x3C000000：不回应
（见 `board/esp32c3/platform.c` 的 `plat_block_read` 头注释）。所以进入 kmain 时
cache 是关的、MMU 是空的。

结论：两个窗口**都得自己拉起来**（SPI_MEM 的 MMU item index/content +
CACHE_SCTRL，可能还要 ROM 的 Cache_Start）。所需寄存器偏移不在本仓库里
（`third_party/esp32c3-soc/` 只有 9 个头，没有 spi_mem_reg.h；Zephyr 的
`common/loader.c` 只留下 `cache_hal_disable/enable` 的调用点，hal 实现未随仓库），
得从 TRM / esp-idf 抄一份，然后照老规矩上探针。

即便 cache 通了，**代码段 XIP 还有三条与现有设计冲突的硬约束**（见
PLATFORM-DESIGN §7 的加载器模型）：

1. **与 PIE 互斥**：clang 的 medany 模型用 `auipc+addi` 访问全局量和字符串，
   链接期就写死了"数据在代码后 D 字节"，不产生重定位，loader 改不了。代码进
   flash、数据留 RAM 只有一条路——**固定地址、零重定位**（esp-idf 模式），
   于是每个程序一份固定 data 区，同程序不能有两个实例，`-fPIC` 也救不了。
2. **littlefs 物理不连续**：XIP 要求代码在 flash 上连续且 64 KiB 页对齐，
   日志结构的文件系统给不了，必须另开一个连续代码分区（打包器生成）。
3. **擦写期间不能取 flash**：写盘走 ROM write + SPI1 硬件擦除，没有任何 cache
   停摆（esp-idf 为此有 `spi_flash_disable_interrupts_caches_and_other_cpu`）。
   现在安全只是因为"写盘只在内核态、向量表和内核都在 SRAM"，XIP 后这条要变成
   硬约束。

**便宜得多的中间态**：只把大常量表（字库/查找表）放进 flash，代码仍在 RAM。
把表链接到一个绝对地址符号（`PROVIDE(xip_rodata = 0x3C000000)`），程序不按
符号名引用它、只经由内核给出的地址访问，因此不需要改 PIE、不需要改窗口模型。
它同样卡在"cache 能不能拉起"这个前提上。

### 4. 无重定位的 rodata 可以留在原地

LLEXT 4.5 新增 `LLEXT_MEM_RODATA_NO_RELOC`：约定一个专用 section，加载时
检查它**没有重定位**就直接指向 flash 原地址，不拷贝。思路可移植：
"能否不搬"的唯一判据是该段有没有 RELATIVE 重定位，而不是段的读写属性。

### 5. PMP 的成熟做法（对照用）

`arch/riscv/core/pmp.c`：cfg 不直接写 CSR，先维护 RAM 影子副本批量回写
（避免 RMW）；每线程预计算 M-mode / U-mode 两套条目，切换时只写寄存器。
我们的每任务四条目在 `sched_switch` 里现算，条目少所以无所谓，但"影子副本"
在条目变多后值得抄。

## 不采纳的部分（以及为什么）

| Zephyr 做法 | TinyOS 取舍 |
|---|---|
| LLEXT 用**符号名**绑定固件导出表 | 保持 ELF 重定位 + 相对寻址：换内核版本不用重编用户程序，这是"同一份二进制跨板分发"的前提 |
| 扩展是"被固件调用的代码"，无调度无退出码 | 用户程序是进程：有窗口、有栈、能崩溃隔离、能 exec/wait |
| 8 种 ELF 形态/加载器（fs/buf/semihost/memblk...） | 只支持文件系统里的一种形态（LZ4TOS 信封），加载路径唯一 |
| CONFIG_LLEXT_HEAP_SIZE 固定堆 | 窗口模型：窗口大小 × 并发数即总占用，无需堆配置 |

## 探针待验证（写进 P3 清单）

1. 0x42000000 取指：把代码段链接到 IROM 窗口，实测 ROM 的 mmap 是否已可用。
2. 0x3C000000 读：烧一个 littlefs 镜像到 flash 偏移 X，从 DROM 窗口 memcpy 读回。
3. flash 大小/ JEDEC ID 经 ROM SPI 函数读取（确定分区表）。