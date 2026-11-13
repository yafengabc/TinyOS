# 待决清单

每次评估完把结论钉在这里：**已落地的写进「已合并」，被否的写清理由**（省得下次重提）。
体积数字都是 `-Oz`、与 `tos build` 逐字节一致的构建测出来的。

逐项结论见下表；§12 / §13 是两次脚本引擎评估的完整实测（Berry 与 MicroPython
都没换，但 §12 指出的那条架构短板当天就补上了）。

| # | 事项 | 结论 | 代价 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | `reboot` 真复位 | 做，五端口全实现 | 未单独计量 | 已合并 `fc08eb2`（C3 20/20） |
| 2 | QEMU memcpy 慢 / RV32EMU 复现 | 不是 bug，是 TCG 每迭代重译 | 无（只影响写法） | 已记录 `b31e854` + §8 |
| 3 | 调度器加优先级 | 值得做，三级 + 可抢占 | x86 `-O2` `.text` +512、rv32 `-Oz` `.text` ±0 | **已合并** `97b07c4` |
| 4 | 大程序 XIP | 用户程序 **不该做**；**内核已做并上板** | 内核省 66.8 KiB RAM | **已完成** 2026-11-06 |
| 5 | 极简 shell 脚本 | 值得做，`source` + 启动脚本 | rv32 sh +576 B 原始 / +703 B 压缩 | **已合并** `7d949a3` |
| 6 | 单文件多入口 vs 共享库 | 多入口 **−55%**，SO **+18%** | 内核 +~50 B | 只评估，已写进 §7.1（`d681089`） |
| 7 | 每程序栈（默认 8 KiB + `TOS_STACK`） | 做，ELF note 通道，PMP 规则数不变 | 内核 +~60 行；小工具窗口 32→16 KiB | **已合并**（`PLATFORM-DESIGN.md` §8.9.1） |
| 8 | 每程序堆（`TOS_HEAP`，note type=2） | 做，与栈同一个通道，无 2 的幂约束（TOR） | 窗口公式 + 堆一项；脚本引擎直接受益 | **已合并**（`PLATFORM-DESIGN.md` §8.9.3） |
| 9 | 控制台传文件（`tosput` + `rx`） | 做，行协议 + 停等 + 板端 CRC32，零内核改动 | `tools/tosput` + `user/rx.c` | **已合并**（`docs/FILE-TRANSFER.md`） |
| 10 | 串口 RX 突发上限 ~128 B/tick | **原诊断错了**：UART0 接收早已中断驱动；真凶是 `usj_putc` 无 USB 主机时每字节空转超时，而内核打印关中断，把 UART0 排空一起饿死 | 已改成 USJ 发送环 4 KiB + IN_EMPTY 中断、USJ 接收中断、raw 环 1 KiB（按实测缩回 512，见下）、中断里唤醒读者 | **已合并**；吞吐 1.0 → 7.0 KiB/s（理论 8.6）。原生 USB 两路中断未上真机（本机只插 CH343），见 §10 |
| 11 | 传文件窗口化（不等 ACK 连发 N 块） | 未评估：当前 7.0 KiB/s 已是 115200+base64 的 81%，窗口化只能再挤 ~19% | 协议 + 板端乱序/重排逻辑 | 待评估（优先级低） |
| 12 | Python 解释器改字节码 VM | **做**：AST 整体退役，单遍编译 + 栈式 VM | flash +3 KiB，RAM −27%，速度持平 | **已合并**（§13） |
| 13 | 移植 MicroPython，只留 runtime | **不换**：core 76.8 / 54.0 KiB，是我们的 2.17× / 1.53× | 10 个 libc shim + 软浮点 + GC 改造 | 只评估（§13） |

---

## 1. `reboot`（已合并）

`RTC_CNTL_OPTIONS0(0x60008000) bit31` 就是软件复位，只写位不用解锁，ROM 报
`rst:0x3 (RTC_SW_SYS_RST)`——这个字符串进了 `accept.py` 的断言，因为它是「真复位」
和「主机拉 EN」唯一可区分的指纹。复位前必须 `console_shutdown()` 排空控制台：
115200 下最后一个字符还在移位寄存器里（87 µs）。

其余四端口各有各的写法：x86 `outb(0x64, 0xFE)` 走键盘控制器复位线，qemu-virt 写测试设备
`0x7777`，mps2 与 rp2040 写 `AIRCR = 0x05FA0004`（`SYSRESETREQ`，新增 `arch/arm/scs.h`）。
smoke 加了 `-AllowReboot`（默认带 `-no-reboot`），中途出现 `init process exited` 即判 FAIL。

**剩余**：rp2040 那条是照 ARMv6-M 抄的、真机没验（Pico 没接控制台）；复位后 FS 里
`wfile` 写的文件仍在（C3 已验证）。

## 2. QEMU 的 memcpy / RV32EMU 的对照（已记录）

同一份 `user/bench.c`（4 KiB × 64 趟 = 256 KiB 载荷）在两个模拟器上跑：

| 循环 | 每趟 | RV32EMU | QEMU rv32 | 比值 |
| --- | --- | --- | --- | --- |
| 逐字节 `lbu/sb` | 5 条 / 1 B | 39.5 ns | 12 278 ns | 311× |
| 逐字 `c.lw/c.sw` | 4 条 / 4 B | 37.4 ns | 12 075 ns | 323× |

结论：QEMU 的代价按**迭代**算（1 B 和 4 B 都是 12 µs），是 TB 每次迭代被重译的指纹；
RV32EMU 是纯解释器，代价随指令数走、±2% 确定。所以这是「写代码的指导」而不是 bug：
热路径按字拷贝。顺带量到 RV32EMU 上 U-mode 8–9 ns/指令 vs M-mode 3.6（每条访存退回
`Step()` 做 PMP 检查）。

## 3. 调度器优先级（已合并 `97b07c4`）

三级（0/1/2）、同级轮转、唤醒即 `need_resched`、`nice <n> <prog>` 与 `prio <pid> <n>`。
`ps` 多一列 prio。冒烟测试 x86/rv32 全过。

**代价**（合并后复测）：x86 `-O2` `.text` +512 B / `.rodata` +12 / `.bss` +64；
rv32 `-Oz` 下 `.text` 恰好持平（28,936 B，代码不同但总量相抵——新增的
`make_ready`/`preempt_if_higher`/`proc_by_pid` 出线被简化掉的 tick 路径抵掉），
`.rodata` +12、`.bss` +128；sh 3,688→4,264 原始 / 2,656→3,064 进镜像。

**测法上的坑**：第一次测「优先级是否生效」时前台任务继承了 shell 的级别 0，秒杀后台，
看起来像没实现；改成同级对比才测出 50/50。饿死是**有意行为**，不是 bug。

**合并时的坑**：`.o` 不分目标共享——构建 x86 会覆盖 rv32 的 `build/kernel/*.o`，
量完 x86 再量 rv32 前必须重新构建，否则你是在拿 x86 的中间产物给 rv32 报数
（本次就撞上了：REL vs RELA 重定位直接暴露了架构不对）。

## 4. 大程序 XIP（用户程序：不该做；内核：已做，见本节末）

想解决的问题：程序一旦大于窗口放不下，就不能跑。XIP 让代码留在 flash 上执行。
四条硬约束，每条都和现有模型冲突：

1. **与 PIE 互斥**：medany 用 `auipc+addi` 访问全局量，链接期就把「数据在代码后 D 字节」
   写死了、不产生重定位，loader 改不了。代码进 flash、数据留 RAM 只有固定地址零重定位
   一条路 → 每程序一份固定 data 区、同程序不能双实例、每任务窗口作废。
2. **littlefs 给不了连续 64 KiB 页**，得另开一个连续代码分区（且只能不压缩）。
3. **擦写期间不能取 flash**：写盘现在安全只因为「写盘只在内核态、向量表和内核都在
   SRAM」，XIP 后这条要变成硬约束。
4. 前置条件本身就没验：C3 的 flash cache/MMU 是二级引导建立的，我们绕过了二级引导，
   `drom.S` 在真机上读 `0x3C000000` 不回应 → cache 是关的。要自己拉 SPI_MEM 的
   MMU item + `CACHE_SCTRL`，寄存器偏移不在本仓库里。

**便宜的中间态**：只把大常量表（字库/查找表）放 flash，代码仍在 RAM——表链接到绝对地址
符号、程序只经内核给的地址访问，不改 PIE 不改窗口模型。同样卡在「cache 能不能拉起」。

**内核 XIP 是另一个命题，已经做完并上板（2026-11-06）。** 上面四条里前两条只对
*用户程序* 成立：内核是固定地址、非 PIE，镜像本来就在 flash offset 0，连续性问题
不存在。剩下的两条都解决了，过程如下，都是真机实测。

### 怎么拉起来的（`board/esp32c3/kernel-xip.ld` + `arch/rv32/start.S` 的启动桩）

1. **cache/MMU 得自己拉，但 ROM 的 helper 全是假的。**
   `Cache_Ibus_MMU_Set`、`Cache_Dbus_MMU_Set`、`spi_flash_enable_cache`、
   `Cache_MMU_Init` + `Cache_Owner_Init` + `Cache_Set_IDROM_MMU_Size` 全部返回 0，
   窗口内容一个字节不变——之前读到的「app 分区数据」其实是 ROM 留下的陈旧 cache 行。
   **正解是直接写 MMU 表**：基址 `0x600C5000`，4 字节一项，64 KiB 一页，
   **项值 = 物理地址 / 64 KiB**（不是字节地址！写成 `0x40000` 会让窗口指向页号
   0x40000，取指永远不完成），`0x100` = 未映射。写完 `Cache_Invalidate_ICache_All`
   (0x400004d8)。
2. **两个门控位复位值都是「关」**：`EXTMEM_ICACHE_CTRL`(0x600C4000) bit0 使能、
   复位 0；`EXTMEM_ICACHE_CTRL1`(0x600C4004) bit0/1 = SHUT_IBUS/DBUS、复位 1。
   只写表不置这两位，第一个取指就挂死（数据读却能过，所以探针看起来是「成功」的）。
3. **`mtvec` 必须在任何可能 trap 之前交还给真表**：C3 硬性 vectored 模式，
   base + 4*cause，桩里若还留着调试处理器，第一个 tick 就跳到指令中间。
4. **`0x42000000` 的段不能当 ROM 加载段**（那是 flash 的只读别名）。代码作为裸字节
   `XipBlob` 追加在镜像 `XipOffset`（64 KiB 对齐）处，由启动桩映射。
   `internal/build/espimage.go` 的 `XipBlob`/`XipOffset` + `xipBlob()` 负责按
   `vaddr - 0x42000000` 排布，含单测（`TestESPImageXipBlob*`、`TestXipBlobLayout`）。
5. **擦写期间不能取指**：程序/擦除路径现在在 `plat_block_write/erase` 入口关中断
   （`flash_irq_off()` 保存 mstatus.MIE 并精确恢复）。erase 的忙等循环体在 icache 里
   已经热了，所以能跑完；真正的风险是 ROM 驱动 SPI 控制器时被打断。
   实测 800 KiB 写入 + 读回 `pattern verified` 通过。

### 实测数字（ATK 板之外的这块 C3 核心板，COM18）

| | 之前（全 RAM） | 现在（XIP） |
|---|---|---|
| 内核 RAM 结束地址 | `0x403aae60` | `0x4039b4e0` |
| 176 KiB 窗口余量 | 4,512 B | **66.8 KiB** |
| 内核占用（`free`） | 175.6 KiB | **109 KiB** |
| 代码位置 | SRAM | flash `0x40000` 起，止于 `0x42010248` |

开机第一行自检：`[xip] mmu[0]=4 want 4, window: 41 11 6 c6`（4 = 0x40000/64 KiB，
后四个字节是 `.text` 的头 4 字节）。`df` / `ls` / `cat` / 写文件 / `dd` 800 KiB
读回校验 / `uptime` 全部正常。

**收益兑现**：省下的 63.7 KiB 正是当初想给脚本引擎的地方；窗口暂时仍留在 176 KiB，
余量先记在账上，需要时再缩到 128 KiB。

## 5. 极简 shell 脚本（已合并 `7d949a3`）

`src_t {fd,pos,len}` + `src_line()`（`-1`=EOF / `0`=空行）+ 进程内递归的 `run_script()`
+ `source` 命令（深度上限 4）+ `files/etc/rc` 启动脚本（只有 init 即 pid 1 执行）。

**真实约束**：C3 只有 3 个任务窗口（shell / 前台 / 备用），所以嵌套 `run sh` 必爆
`exec: out of task windows`——这就是为什么用 `source`（进程内递归）而不是「脚本就是
一串 exec」。这条在 QEMU 上完全看不到。

**踩过的坑**：`src_line` 一开始用同一个 `0` 同时表示 EOF 和空行，于是每个脚本在第一个
空行结束（`etc/rc` 第 7 行）；量体积时误用了 ARM 的 `build/user/bin/sh` 做跨架构对比，
得出假结论「+193 B」，重测才是 rv32 sh 2,680→3,688 原始 / 1,953→2,656 压缩（+703 B）。

**合并时收的尾**：`etc/rc` 改成纯注释（开机自己说话的机器没法做差分测试）；
`source`/`run sh` 打不开文件时点名报错（原先两条路径都静默）；README 新增「脚本」一节、
PLATFORM-DESIGN 新增 §6.5；`accept.py` 加了 4 条用例（wfile 写脚本 → source 跑通、
点名报错、`run sh etc/rc`）。rv32 + x86 smoke 全过。

> **2026-11-04 扩展成脚本语言**：`$(prog)` 经 pipe 取回 stdout、`$var`/`$?`、
> `if/else/fi` + `while/done/break` + `incr`、`true/false`，脚本内 `sleep`/`echo`
> 内建。为此把整份脚本读进 4 KiB 静态池（`source` 追加、返回时退回池指针），
> 按行号跳转，不再用 `src_line()` 的流式文件读——因为内核没有 lseek，
> `while` 没有别的办法回到开头。`bin/sh` 从 7.0 → 17.7 KiB ELF
> （LZ4TOS 后 14.5 KiB，x86 实测），窗口仍 32 KiB，跑完 demo.sh 的栈高水位
> 1.6/8 KiB。**新踩的坑**：内建 sleep 会停住 init，而 `process_signal()` 明确
> 拒绝 init，所以 `^C` 收不回来；最终把内建严格限制在脚本内，控制台仍走
> `bin/sleep`，实测 0x03 立刻 `[pid N] killed`。回归夹具 `files/etc/demo.sh`
> 由 `tostest emutest` 的 `script` 检查覆盖。详见 `SHELL-GUIDE.md` §1。

## 6. 单文件多入口 vs 共享库（只评估，见 PLATFORM-DESIGN §7.1）

> **2026-11-02 复测并补测**：数字已更新（树变了：sh 从 2,680 长到 4,264 B），
> 并且补了三个之前没做的实验 —— **SO 在真内核上跑不起来**、**分组打包比全塞一个更省**、
> **exec 的解压缓冲永不释放**。详见下面的「复测」小节。

### 第一轮（合并前）

| 形态 | 原始 | LZ4TOS 后 |
| --- | --- | --- |
| 14 个独立 ELF | 20,756 | 14,572 |
| 单文件多入口 | 8,776 | **6,548（−55%）** |
| 共享库（4,400 B 库 + 14 个动态程序） | 27,012 | 17,152（**+18%**） |

关键：空程序 716 B 里 `.text` 只有 22 B，逐字节重复的代码只有 2,946 B —— 省下的是**包装**，
不是共享代码。所以 SO 必输（判据 `N·u > L + N·g`：现在 2,946 对 6,256）。已在 rv32 QEMU
上真跑通（`busybox hello alpha beta` 等，SMOKE OK），dispatch ≈ 440 B。

### 复测（2026-11-02，当前树，rv32imc -Oz，同 `tos build` 命令行）

| 形态 | 原始 | LZ4TOS 后 | 相对独立版 |
| --- | --- | --- | --- |
| 14 个独立 ELF | 21,332 | 14,994 | — |
| 14 个独立，去掉段头表 | 17,692 | 13,180 | −12.1% |
| **单文件多入口（14 applet）** | 9,352 | 6,953 | **−53.6%** |
| **分组打包（2 组：core 6 个 + tools 7 个）** | 7,648 | **5,879** | **−60.8%** |
| 共享库（4,436 B 库 + 14 个动态程序） | 27,672 | 17,610 | **+17.5%** |

**分组打包为什么比全塞一个更省**：组内 applet 共享度高，压缩率反而更高（76% vs 74%）；
全塞一个时，不同 applet 的重复字节互相掩盖，LZ4 找不到更长的匹配。

**分组打包已真跑通**（rv32 QEMU）：`run core ls` / `run core echo hi` / `run core cat readme.txt`
/ `run core uptime` / `run core mem` / `run tools hello alpha beta` / `run tools hello_printf x y`
全部正确，`argv[0]` 被改写成 applet 名（`hello` 而不是 `tools`）。

### 三个新发现

1. **SO 在当前内核上根本跑不起来**（第一轮只测了体积，没试过能不能跑）。
   实测把 SO 版 `hello` 放进 FS 执行 → `[pid 2] instruction access fault at 0x000000c0`。
   根因：SO 用 `R_RISCV_JUMP_SLOT` + `.dynsym` 做绑定，而 `kernel/elf.c` **不解析动态符号表**
   —— 它把 JUMP_SLOT 当成「本地函数」（S=0），算出 `addend + bias` 写进 GOT。
   SO 的 JUMP_SLOT addend 是 0，于是 GOT 里塞的是加载基址，跳过去是垃圾。
   `0xc0` 正是 `.plt` 段起始 vaddr（PLT0 解析桩）。
   → 结论从「SO 体积亏 18%」升级为「SO 要先给内核写一个动态链接器」。

2. **`exec` 的解压缓冲不泄漏**（上一版此处写的是"从不释放"，已订正为相反的结论）。
   `kernel/process.c` 第 582 行 `data = raw` 之后，第 630 行的 `ramdisk_release(data)`
   释放的就是 `raw`（`ramdisk_release()` = `kfree()`）；失败路径也各自配平。
   实测：临时加 `heap_used_bytes()` 探针，在解压后 / 释放前 / 释放后 / proc_create
   末尾四个点各打一次，**每次 exec 结束后 used 都回到 0**：

   ```
   HP decompress 'bin/sh':     raw=4456 used=4456
   HP before release 'bin/sh': used=4456
   HP after  release 'bin/sh': used=0
   HP proc_create done 'bin/sh': used=0 peak=7664
   ```

   独立程序与分组包各跑一遍都是如此。上一版的错误在于探针只打一个点（`raw` 刚分配
   还没释放），把**瞬时占用**当成了**累计占用**。量泄漏必须看事件前后的差值。

3. **多入口的另一个隐性成本**：每次 exec 都要解压**整块**，哪怕只想跑 `ls`。
   分组打包把这个代价从「全部 14 个」降到「6–7 个」，比全塞一个更划算。

### 结论

- **共享库**：否决。体积 +17.5%，而且当前内核不支持 —— 要先写动态链接器、
  处理无 MMU 下的固定地址共享、库 `.bss` 跨任务共享（`printf` 的 512 B outbuf 会互踩）。
- **多入口**：值得做，**分组比全塞一个更好**（−60.8% vs −53.6%）。代价是 exec 峰值
  内存，分组把它从 9,352 B 压到 2,888–4,760 B。

**待定**：上不上？如果上，推荐**分组打包**而不是全塞一个。
另外那条正交的零架构改动**已做**（2026-11-06，见 §16）：段头表 + `.shstrtab`
一起裁掉，实测 36 个程序省 12 088 B 原始 / 7 149 B LZ4TOS。

---

## 还没结的尾巴

- **~~littlefs 在 SMP 下没有互斥~~**（2026-11-03 已做）：`kernel/ramdisk.c` 加了一把
  `fs_lock`，粒度是“一次 API 调用”（`cfg` 缓存 + bounce 缓冲 + 句柄表 + readdir
  快照都是全局的，按块加锁不够）。单核上它编译成空操作，因为 tick 不会在内核态
  切换任务；`readdir` 的名字改成拷进调用者缓冲，否则指针在锁外就失效了。
  见 `PLATFORM-DESIGN.md` §13.5。
- **~~`/dev` 和 `/proc` 只能 open，不能 `ls`~~**（2026-11-03 已做）：目录改成从
  设备表算出来的（`devfs_dir_count/_dir_name`），`SYS_READDIR` 按前缀路由，
  `stat` 新增 `TOS_FTYPE_CHR`，`cd /dev` 也通了。见 `PLATFORM-DESIGN.md` §14.7。
  剩下的小缺口：`ls /dev` 在没注册任何设备的板子（qemu-virt、pc）上是空目录，
  这是对的；但设备节点没有 size 概念，`ls` 显示 `<dev>` 而不是字节数——够用。
- **~~arm(qemu-mps2) 首次 exec 就 HardFault~~**（2026-11-05 已修/不再复现）：
  `tostest compat -targets x86,rv32,arm` 全绿，含 `hello`/`hello_printf`（exec 用户
  ELF）、`cat readme.txt`（littlefs）、`run prot *` 全组；`smoke -target arm` 默认
  14 条命令序列也过。当时的两个症状都不在了。
  附带修复了 mps2 的串口交错假象：mps2 QEMU 的 PL011 只有 16 字节 RX FIFO 且无
  流控，整行命令一次写入会溢出、回显丢字符——`runSmoke` 现在逐字节节流发送
  （3 ms/字节）并在每条命令后等下一个 `tos> `，`compat` 对残余的顺序抖动用
  「字符多重集相同则记 ok*」的可见回退兜底。
- **~~arm(qemu-mps2) 又回到“首次 exec 就 fault”~~**（2026-11-06 修）：这次不是 ELF，
  是 **MPU 区域预算**。ARMv7-M 的区域必须是自然对齐的 2 的幂，`wx` 不对齐时切块数
  ≈ 起点/终点各自的零散位数；`sh` 涨到 21 KiB 之后要 **4+9=13 块**，而预算只有 7，
  旧代码在 `mpu_area()` 里静默截断，窗口上半（栈）没区域覆盖 → 任务在入口
  `win_base+1` IACCVIOL。现在先数块（`mpu_blocks()`），装不下就退化成整窗 RWX 并
  打印 `[mpu] window needs 4+9 blocks (budget 7)`；`smoke -target arm` 恢复全绿。
  根因与取舍见 `PLATFORM-DESIGN.md` §5.2。要恢复 M3 的 w^X 得让 `wx` 落在 2 的幂
  偏移上（`user.ld` 的 `.data` 前加 `. = ALIGN(2^k)`，构建器探针链接后回填），
  代价是镜像最多翻倍——M33 的 PMSAv8 没有这个限制，所以暂时不做。
- **~~RP2350 上 `reboot` 之后板子挂住~~**（2026-11-06 修）：`AIRCR.SYSRESETREQ`
  只复位内核和部分外设，USB 控制器/时钟树保持原样，结果主机能枚举 CDC 但打不开
  （`PermissionError 13`）、UART0 全静默。改成走 ROM 的 `reboot`
  （flags `0x100` = NORMAL|NO_RETURN_ON_SUCCESS），现在整条启动日志都在、CDC
  重开即 `tos>`。见 `BOARD-ATK-DNRP2350AM.md` §4。
- **~~一条 fault dump 里第二条任务报第一个任务的地址~~**（2026-11-06 修）：CFSR/
  HFSR/SFSR 是写 1 清零，内核从来不清，`MMARVALID` 一直挂着，于是连着跑四条
  `prot` 都打印第一条的 `mmfar`。现在 fault dump 先快照、再清零、再打印。
  另外 dump 尾部会被 CDC 环形缓冲丢掉（`usb_cdc_putc()` 满了就丢），所以关键
  信息都排在前面。
- **RP2040 / RP2350 的 core1 还没起**：`board/rp2040` 仍是 `PLAT_MAX_CPUS = 1`，
  内核里 SMP 那一整套（per-CPU 栈、第二张向量表、自旋锁）已经就位，缺的是
  板级三件事：SIO 自旋锁（`SIO_SPINLOCK0` 替掉 `plat_spin_acquire`）、
  把 core1 从 bootrom 的 `wfe` 循环里拉起来（`SIO_FIFO` 或 PSM）、
  `plat_timer_start_cpu()` 给 core1 装 SysTick。见 `PLATFORM-DESIGN.md` §13.9。
- **SMP 没有任务亲和性**：一条共享队列，任务会在核之间迁移（`smp` 程序会报
  `cpu 1 then 0`）。要绑核就加 `proc_t.affinity` + `sched_pick()` 过滤，
  属于策略，先没做。
- **C3 的原生 USB（USJ）控制台**：RX 中断和 TX 环都已实现，但本机只插了
  CH343，主机侧没验过（见 §10 尾巴）。
- **SPI1 硬件页编程（PP）没通**：写走 ROM，不阻塞。
- **README 说「默认 `-Oz`」是错的**：`internal/build/build.go` 的 `TOS_OPT` 默认 `-O2`
  （真机才建议 `-Oz`）。改哪一边等你定。
  （2026-11-04 补：全局默认值仍不动，但**按程序**可以单独指定 —— `user/<name>/opt`
  文件里写一个优化级别，只对这个程序生效。Python 解释器用它 `-Oz`：镜像从 35 KB
  降到 27 KB，而 RV32 的 `-Oz` 在 QEMU TCG 下慢 100 倍这件事只跟**代码形状**有关，
  内核保持 `-O2` 就不会踩到。）
- **解释器的 arena 为什么不是 TLSF**（已决，2026-11-04）：TLSF 回答的是"这一块
  内存从哪来"，解释器缺的是另一半"这一块什么时候该还"——那是引用计数。
  引进 TLSF 约 1000 行 / 4-6 KiB flash，会改掉全系统 malloc 的语义（波及
  `bench`/`mem`/`wheap`），而且仍然不解决循环引用。实际做法：解释器自带一个
  arena（一级 size class + bump），配合引用计数；树和 token 用**无块头的永久分配**
  （`py_alloc_perm`），省掉每块 16 字节的块头。详见 README「Python 子集解释器」。
- `0x40380C00 / 0x3FC80C00 / 0x3FC8F000 / sp-256` 处 store→load 读回 `0x60000000`，
  疑似 flash cache 保留区，未解。

## 10. 控制台 RX/TX 与传文件吞吐（已合并，原生 USB 待验）

真机排查过程（2026-11-03，C3/COM13）：

1. 现象：`tosput` 传文件停在 chunk 0；板子回 `NAK 0 r1[...]`，片段从 base64
   第 120 字符开始 = **行首 128 字符丢失**；python 探针同样的字节却全通。
2. 一度怀疑 USJ 中断没使能——回读 `INT_ENA=4`（bit 2 已开）、`INT_RAW=8`
   （`SERIAL_IN_EMPTY` 上电默认置位，未使能），使能是对的。**顺带发现
   `third_party/esp32c3-soc/interrupts.h` 不是 C3 的表**：它少掉蓝牙那 6 个
   源，整体偏移（UART0 读出来是 15，真机是 21）。真值：UART0=21、USJ=26、
   TG0_T0=32，后两个与真机验证过的 21/32 同间距。
3. 决定性实验：把 `usj_putc()` 临时改成直接 return，10/10 块连续全通。
   所以真凶是**无 USB 主机时 `usj_putc` 每字节空转 40000 次超时**；内核打印
   在 trap 里执行、中断关着，UART0 的接收 FIFO 就这样被饿到溢出。
4. 修复：USJ 发送改成环形缓冲 + `SERIAL_IN_EMPTY` 中断（永不阻塞，
   没有主机就填满即丢）；USJ 接收中断（`SERIAL_OUT_RECV_PKT`）；raw 环
   256 B → 1 KiB、cooked 1 KiB 字节池；`process_console_kick()` 让接收中断
   直接唤醒阻塞的读者，而不是等下一个 tick。
5. 结果：64 KiB 传输 64 s → 9.4 s，**1.0 → 7.0 KiB/s**；`accept.py` 24/24。
6. 收尾：各队列按实测约束收缩。raw 环按"最坏调度延迟 = 一个 QUANTUM，
   当时是 5 tick = 50 ms，115200 只走 576 B"定到 1 KiB；`QUANTUM` 改到 1 ms 后
   这个理由只剩 12 B，2026-11-04 直接量了一次（`user/rawsink.c` +
   `scripts/esp-c3/rawstorm.py`：读者睡过一个 4 KiB 突发再排空，捞回多少就是
   多少），512 → 511 B、1024 → 1023 B，差值恰好 512，**硬件 FIFO 不叠加**
   （中断一直在把 FIFO 搬进环）。缩到 **512 B**：stop-and-wait 下 `rx` 是
   `write()` 完才回 ACK，写 flash 期间线上没有数据，44 ms 的窗口足够；
   64 KiB 传输在 512 下 3/3 verified，7.7 KiB/s 不变。USJ 发送环 4 KiB 是按"没人插 USB"
   设的，缩到 1 KiB 无影响；cooked 行队列改成 1 KiB 字节池（2 字节长度 +
   内容），短命令只占 9 字节，实测粘 100 行全跑完。顺带修了三个真 bug：
   环形判满用"空槽当满"白丢一个槽位；`\r\n` 被当成两个换行（粘 12 行 CRLF
   只跑 5 行，修后与 `\n` 一致）。
7. **粘贴 200 B 以上就丢行、甚至整块控制台装聋**（本轮最大的发现）：内核在
   trap 里关着中断，`exec` 读 littlefs 是几十毫秒的盲窗，而 128 B 的接收
   FIFO 只存 11 ms 的输入。溢出之后 C3 的 UART **不再交付也不再报中断**——
   症状是粘贴跑到第 20 行然后控制台彻底没反应（tick 和任务都还在跑，所以很
   容易误判成队列问题）。修法两层：溢出中断使能 + ISR/tick 里查 `INT_RAW`
   的 OVF 位并复位接收 FIFO（止血）；`bd_read`/`bd_prog`/`bd_erase` 每次
   调用前 `tty_poll()`，把盲窗从"整个 exec"降到"一个扇区"（除根）。
   实测 24 行 4/24 → **24/24**、40 行 → **40/40**、100 行 → **100/100**，
   控制台全程存活。C3 `.bss` 105 984 → 96 720 B。

8. **`NAK r1` 又出现，但这次不是内核（2026-11-04，C3/COM4）**：1 KiB 块重新
   开始在 chunk 0 卡住，症状依旧是"片段从 base64 中间开始"。为了排除内核，
   临时在 `tty.c`/`platform.c` 加了计数并在 raw 模式结束时打印一行：

   ```
   [raw] in=1377 drop=0 take=1377 | uart bytes=1378 irq=38 ovf=0
   ```

   1378 字节经 38 次接收中断全部排空，raw 环进 1377 全部交出、`drop=0`，
   硬件 FIFO 溢出计数 `ovf=0` —— **内核一字节都没丢**。把 `RAW_Q` 从 1 KiB
   改到 2 KiB 也没有任何变化（2048 仍丢同样的位置），说明和环深无关。
   真因是 `flash.py --keep-fs` 保住了**旧的文件系统镜像**：板上的 `rx` 还是
   `8310a29` 的 `CHUNK 512 / LINE_MAX 768`，`read_line()` 在第 768 个字符处
   返回 −1 且 `expected_seq==0` 时**静默不回复**，剩下的 609 字符从 base64
   中间被当成新的一行 → `NAK 0 r1`。**768 就是旧 `LINE_MAX`，一字节不差。**
   修法：`READY` 带上块大小当协议版本，两个宿主工具握手时比对，不一致直接
   报"stale filesystem image: flash without --keep-fs"；`NAK r1` 补 `n=<行长>`。
   两个方向都在真机验过（旧 `rx` 上确实一行报错就停）。


**尾巴**：本机只插了 CH343（COM4 = UART0），原生 USB 口没线，所以 USJ 的
RX 中断和 TX 环只在代码层对称实现，没在真机跑到主机侧。插线后要验：用新出现
的 COM 口传一遍文件；`cat` 一个大文件确认输出不缺字；`INT_RAW` 里
`SERIAL_IN_EMPTY` 是否按包触发（若 TX 环不排空，症状是**只在该口静默**，
UART0 不受影响）。

~~**顺带发现、尚未处理**：C3 的 tick 实测约 **30 Hz**~~ → **已修（2026-11-04）**：
不是丢 tick，是时钟假设错。本移植从不配置 PLL，芯片停在硬件复位的
**APB = 40 MHz 晶振 ÷ 2 = 20 MHz**（ESP-IDF 的 160 MHz 是二级 bootloader 切的），
而代码按 80 MHz 算分频。顺带发现 `T0_DIVIDER` 字段存的是**除数本身**（不是除数−1），
两处一起改：`APB_CLK_HZ=20 MHz`、`TIMG_DIVIDER=20`、字段写 `20`。实测 tick
**100.1 Hz**（15 s 窗口比值 0.9989）。推导与三个独立仪器的读数见
PLATFORM-DESIGN §8.6「时钟：这颗芯片只跑 20 MHz」。

## 11. `tostest bench` 在 esp32c3 上跑不了（2026-11-04 发现，待定）

现象：`bench` 和 `bench 4 1` 都打印 `bench: out of memory`。

原因不是模拟器，是窗口：C3 的任务窗口按 16 KiB 步进，镜像向上对齐到 4 KiB 页，
8 KiB 默认栈 + 4 KiB guard 之后只剩 4 KiB，而 2.1 KiB 的 `bench` 镜像对齐后正好
占满 → sbrk 堆为 0，`malloc(4)` 就失败。`bench` 是少数真的 `malloc` 的用户程序，
别的（`ls`/`cat`/`hello`/`hold`）都不用堆，所以没暴露。`-clk wall` 与否无关。

而且修不了太大的：`tostest bench` 的默认负载是 4 KiB × 64 = 256 KiB，两缓冲区
512 KiB，而 C3 的用户区总共只有 192 KiB（`APP_MAX - APP_BASE`），`TOS_HEAP` 也
救不回来。选项：(a) `tostest bench` 只用于窗口更大的目标（qemu-virt 64 KiB /
rp2040），C3 上直接报错说明原因而不是撞 OOM；(b) 给 `bench` 换一套小负载的
参数并在 C3 上声明 `TOS_HEAP`。

性能基准目前用 rv32emu-c 自带的 `rv32emu-bench.exe`（`check` / `tinyos`），
它在宿主侧计量，不受这个限制。

## 12. Berry 评估：要不要换成现成的嵌入式脚本语言（2026-11-04，实测）

问的是「评估一下 berry 脚本」。Berry（`berry-lang/berry`，MIT，1103 次提交，
最后提交 2026-10-10）是真实存在的嵌入式脚本语言：**单遍编译器 + 寄存器式 VM**，
ANSI C99 写的，语法像 Python（Lua 血统，`end` 收尾、`map` 不是 `dict`、
`for x : list`），有类（单继承 + 运算符重载）、闭包（upvalue）、lambda、
异常（raise/try/catch）、模块系统、**标记-清除 GC**、字节码文件（可固化进
flash）、C FFI。生态上 Tasmota32（ESP32）每月 20 万次下载，M5Stack 在用。

### 不引用它的数字，自己按 C3 的目标编一遍

它 README 说「解释器核心 < 40 KiB」，那是 ARMCC + Cortex-M4 + Thumb 的测量。
按我们的目标（rv32imc、clang、`-Oz`、freestanding）实测：

| | Berry（实测） | 我们的 `py`（实测） |
| --- | --- | --- |
| 源码行数 | 24,416 | ~5,500 |
| 核心 `.text` + `.rodata` | **95.7 KiB**（`-O2` 反而 116.7 KiB） | **24.9 KiB** |
| 依赖的 hosted libc 头 | 11 个（assert/ctype/limits/math/setjmp/stdarg/stdio/stdlib/string/inttypes/time） | 0 |
| 软浮点 | 需要 compiler-rt 的 `__addsf3` 等约 20 个 | 不需要（整数 32 位） |
| 垃圾回收 | 标记-清除（能收循环引用） | 引用计数（循环引用泄漏，有意） |
| 22 行脚本的程序体积 | **605 B 字节码**（可固化进 flash → 0 RAM） | **~8 KiB** AST+token（常驻 RAM） |
| 类 / 闭包 / 模块 / 异常 | 有 | 无 |

移植代价比「写个 port」大：为它编出一份 freestanding libc shim 要补 11 个标准头
和一堆符号（memchr/abort/isnan/LLONG_MAX/inttypes.h/time.h…），异常机制要
`setjmp`/`longjmp`，文件 API 要 `be_fopen` 一族，浮点要 compiler-rt 的软浮点 ——
**而 TinyOS 连内核都没有软浮点**（`kernel/libgcc.c` 只有 329 行整数辅助），
用户态连 `assert.h` 都没有。Berry 默认 `BE_INTGER_TYPE=2`（long long）和
`BE_USE_SINGLE_FLOAT=0`（double），这两个默认值在我们三个目标上都得改；
即使把 `BE_USE_FLOATS` 关掉，`be_code.c` 的 `exp2const` 仍会拉进 `__eqdf2`。

### 结论：不换，但有一条该抄

**不换的理由**：
1. **体积**：核心 95.7 KiB 是我们的 3.8 倍。C3 用户区一共 192 KiB，Berry 光镜像
   就吃掉一半（96 KiB），加上 8 KiB 栈 + 4 KiB 洞，堆只剩约 4 KiB —— 正好是它
   宣传的「< 4 KiB heap」，但那样只够跑很小的脚本，而且和 shell（32 KiB）、
   其他任务挤在一起。我们的 27.6 KiB 镜像 + 96 KiB arena 是能共存的设计。
2. **语法不是 Python**：`end` 收尾、`map`、`for x : list`、没有缩进块。用户要的是
   Python 子集，换过去等于换一门语言。
3. **它的强项（类、闭包、模块、GC、浮点）恰是这个尺寸下最不需要的**；而它的
   软浮点和 hosted libc 依赖是这个尺寸下最负担不起的。

**该抄的一条**：**单遍编译器直接产字节码，根本不建 AST**。这是我们最大的短板，
而且是架构级的、不是优化细节：我们 100 行脚本的 AST+token 常驻 45 KiB RAM，
Berry 是 605 B 字节码（固化进 flash 后 RAM 占用为 0）。其次是寄存器式 VM
（算术不反复 push/pop，我们的树遍历每个节点都是一次函数调用），再次是常量
在编译期构造好放只读段（我们的字符串字面量每个都是一次堆分配且永不释放）。

**不抄的**：标记-清除 GC（引用计数够用，循环引用泄漏是有意的：脚本进程退出时
窗口整个收回）、浮点（C3 无 FPU）。

**如果一定要用**：只能当用户程序（`bin/berry`），需要写一个 TinyOS port
（`be_os_malloc` → 我们的 arena、`be_writebuffer` → `write(1,…)`、
`be_fopen` 一族 → `open/read`、`be_readstring` → 读控制台、异常 → `setjmp`），
浮点必须关掉并补软浮点或改 `exp2const`，窗口按 96 KiB 镜像算，脚本要在主机上
先编成 `.bc` 放进 `files/`（正好契合「ramdisk 里放脚本」这个已有模式）。

**给下一步的启示**：想把解释器在 C3 上跑更长的脚本，第一个动作不是把
`py_node` 从 48 字节压到 36，而是**改成编译到字节码**（树只活在编译期，
跑的时候只有字节码）。这是 Berry 唯一值得抄的地方。

## 13. Python 解释器改字节码 VM + MicroPython 评估（2026-11-04，实测，已落地）

§12 里「该抄的一条」当天就抄了：`py_ast.h` / `py_parse.c` / `py_eval.c` 整体
退役，换成单遍编译器 `py_compile.c` + 栈式 VM `py_vm.c`。源码 → token 数组 →
4 字节定长指令（`op:u8 | a:u8 | b:u16`）→ 执行，**AST 根本不存在**；每个 `def`
是独立 code object，跳转在函数体编译完统一回填。词法器也改成一次分配 token
数组，不再每个 token 一块。

有一条语义约束是抄不来的：这个子集是**动态作用域**（名字在哪绑定就改哪），
编译期无法静态解析成槽号，所以字节码带的是**名称索引**，查找在 VM 运行时沿
作用域链做。内建名也必须 intern 进同一张名字表（作用域按指针比较），否则
脚本里的 `print` 和内建表的静态字符串不是同一个指针 → `NameError`。

同时按同样的 target（rv32imc、clang、`-Oz`、freestanding）实测了 MicroPython
（报告 `/tmp/mp-eval/SIZE-REPORT.md`）：

| 档 | `.text` + `.rodata` | 相对我们的 core |
| --- | --- | --- |
| A：带编译器，设备端跑 `.py` | 78 637 B（76.8 KiB） | 2.17× |
| B：只跑字节码，`.mpy` 从 flash 加载 | 55 277 B（54.0 KiB） | 1.53× |
| 等价脚本 `.mpy` | 279 B | 我们的 4 字节定长指令更胖 |

B 档还要补 10 个 hosted libc shim 头、约 20 个软浮点辅助（`__addsf3` 等），
以及 GC 对「堆可扫描」的要求。**结论：不移植 MicroPython**，自研 VM 是更便宜
的那条路；Berry（§12）同理。

### A/B（ESP32-C3，`rv32emu-c`，`py -v demo.py`；旧版 = 改造前的树遍历版）

| | 树遍历（旧） | 字节码 VM（新） | 变化 |
| --- | --- | --- | --- |
| `demo.py` arena 逻辑峰值 | 59 515 B | 38 667 B | **−35%** |
| `demo.py` arena 物理占用 | 82 568 B | 60 712 B | **−27%** |
| `demo.py` 存活块 | 933 | 158 | **−83%** |
| `demo.py` 程序表示 | AST + token 常驻数十 KiB | 509 条 / 2036 B / 5 个 code object | — |
| `bin/py` ELF `.text` | 26 024 B | 29 944 B | +3 920 B |
| `bin/py` LZ4TOS 压缩后 | 20 510 B | 23 485 B | +2 975 B |
| 宿主 `-O2` 3 万次循环 | 19.4 ms | 20.1 ms | 基本持平 |
| C3 内核堆 | 48 KiB | 56 KiB | +8 KiB |

**省的是 RAM，付的是 flash**。速度没赢：名称查找（动态作用域）是两版共同的
大头，栈式 VM 的 switch 循环没把它变便宜。内核堆涨 8 KiB 是因为 `exec` 要把
解压后的整份镜像装进内核堆再拷进任务窗口，镜像 27.6 → 31.5 KiB；那 8 KiB
落在 `.bss` 末尾到第一个任务窗口之间本来就空着的 SRAM 里。

### 过程中修掉的真 bug（都留了代码注释）

1. `emit()` 的容量单位混了字节和指令条数，每写满 16 条就往缓冲区外溢出
   3/4，把后面块的内存头写坏 —— 名字表被「覆盖」成空串只是它的一次显形。
2. 标签从 0 编号，`label 0` 被当成「无效」哨兵丢掉，第一个 `for` 的循环头
   没落位，回跳距离算成 0。
3. `py_comp` 内嵌 5 KiB 的 label/jump 表，`py_compile()` 和 `compile_def()`
   各在栈上放一份，加上表达式递归压爆 C3 的 16 KiB 栈 → 改 arena 分配
   （`py_alloc` 的 oversize 列表让每个 `def` 复用同一块）。
4. `claim_heap()` 贪心把 brk 顶到堆顶，`process_stack_grow()` 的
   `brk + GUARD > low` 永远成立，栈自动增长对 py 失效 → 32 KiB 起步 +
   arena 按需 4 KiB 增长。
5. 下标赋值 / `del a[i]` 漏压容器、增广赋值求值顺序反了、关键字参数名字
   取自错误的表、函数实参被释放两次（作用域已接管一份引用）。

### 回归

三份脚本（`demo.py` / `pytest.py` 54 条 / `pytest2.py` 21 条）输出与改造前的
黄金基线**逐字节一致**；`tostest emutest` 在 esp32c3 上 `EMU TEST OK`；
7 个目标全部 `build ok`。

## 14. RP2040：这个目标从未在真机上跑过（2026-11-06 记录）

`go run ./cmd/tos build -target rp2040` 能出 `build/kernel-rp2040.elf`、
`build/ramdisk-rp2040.bin`、`build/rp2040.uf2`（1.66 MB），但**这些产物一次都没
烧进过 Pico**。板子那边是空的（"Pico 没接控制台"），所以从 ROM 校验 boot2 到
最后一行 shell 提示符，整条链路都是纸面推演。文档里所有 rp2040 的数字
（窗口/镜像预算、`prot` 四条全 `survived`、`reboot` 走 AIRCR）都按 ARMv6-M
推算，不是实测。

第一次上板要按这个顺序验，前面的不过就别往下走：

1. **boot2**（`board/rp2040/boot2.bin`，256 B）：ROM 会对前 252 字节算
   CRC-32/MPEG-2 并要求等于末 4 字节，不符就**静默不启动**（USB 不枚举、UART 无
   输出，看起来像"板子坏了"）。先单独验这一段：在 boot2 之后立刻打印一个字符，
   确认 `0x10000100` 被跳到。
2. **UF2 family**：`0xe48bff56`。拖拽后盘符消失 ≠ 启动成功（RP2350 那次就吃过
   这个亏：盘符消失了但固件没跑）。
3. **时钟**：现在是"XOSC 12 MHz，UART 分频和 SysTick 都按这个算"，没有像
   RP2350 那样用 FC0 实测。第一件事就是把实测值打出来——不然波特率错了会表现为
   **乱码而不是无输出**，很容易误判成驱动问题。
4. **控制台**：`drivers/usb/rp2040/usb_cdc.c` 是手写 CDC（RP2350 后来换成 TinyUSB 了），
   这段代码从没跑过。先用 UART0（GPIO0/1 + USB-TTL，115200）接通，再接 USB。
5. **XIP vs SRAM**：`kernel.ld` 把内核链在 `0x10000100` 就地执行，`.data/.bss`
   在 SRAM；文档 §RP2040 里"或拷到 SRAM 运行"那句只是选项，**没有实现**。
6. **ARMv6-M 的三条硬限制**：无 MPU（`prot` 四条都会 `survived`，与 x86 同）、
   无硬件除法（`-Os` + 软件除法）、非对齐访问直接 fault（结构体解析要按字节读）。
7. 跑通之后再回收这张清单，把 `PLATFORM-DESIGN.md` §2 验证程度表里那一行从
   "只有编译通过"改成实测结果。

### 已修（离线查出来的，不需要硬件）

**镜像首部曾经是 ELF 头——boot2 跳过去必死。** `board/rp2040/kernel.ld` 把
`FLASH ORIGIN` 设在 `0x10000100`，而 lld 会为「ELF 头 + 程序头表」另起一个
PT_LOAD，并且把它对齐到 64 KiB 页边界的 `0x10000000`。于是：

```
flash 0x10000000  boot2 (256 B)            ← 正确，ROM 校验通过
flash 0x10000100  7f 45 4c 46 ...          ← 不是向量表，是 "\x7fELF"
flash 0x10000200  MSP=0x20016a78 reset=0x100001c1   ← 向量表被推到这儿
```

boot2 的最后一步是 `ldr sp,[0x10000100]; ldr pc,[0x10000104]`，读到的 MSP 是
0x464c457f，第一条指令就落在未定义地址——**板子连一个字节都不会输出**，看起来
和"UF2 没烧进去""boot2 CRC 不对"完全一样。

`internal/build/uf2.go` 的 `elfToFlashRaw()` 现在做两件事：丢掉「偏移 0 且长度
不超过头部表」的纯元数据段，并把返回值的落点从"flash 基址"改成"段的最小 paddr"，
`emitRP2040UF2()` 按这个偏移摆放（`boot2` 仍然占 0..0x100）。

修完的离线校验（不需要板子，`build/rp2040.uf2` 直接解析）：

| 检查 | 结果 |
|---|---|
| UF2 每个块的三段 magic / family | 全对，`0xe48bff56`，3248 块 |
| `0x10000000..0x100000FF` = boot2，CRC-32/MPEG-2 自校验 | 通过（`d58f0b07`） |
| `0x10000100` 字 0 = 初始 MSP | `0x20016a78`（在 264 KiB SRAM 内） |
| 字 1 = reset（Thumb 位） | `0x100001c1`，落在 `.text` 里，指令是 `movs r0,#0 / msr …` |
| `.data` 的 LMA 在 flash 里、内容非零 | 在 `0x100116a4` |
| littlefs 分区 @ `0x10080000` | 有超级块 |

**这仍然不等于"能启动"**：boot2 内部有没有正确配置 QSPI（拿的到底是 W25Q080 的
序列还是别的 flash 型号）、12 MHz 时钟假设成不成立、USB CDC 枚举——都只有真机能回答。

## 15. C3 真机：回显队列排空时机（2026-11-06，已修）

远端 `9dd076e`（XIP + tty 行编辑 + 双核板级）整片烧到 C3 之后，QEMU 与
`go test` 全过，但真机暴露两个 QEMU/emutest 都抓不到的回归。两者**同一个根因**。

### 回显落在提示符之后（整行粘贴才触发）

`echo first` 回显 `echo` → 输出 `first` → `tos> ` 后才补 ` first`；`history`
回显 `hist` → 列表 → `tos> ory`；`lfs` 甚至 `exec: 'bin/lfs' not found` 先于
命令回显；200 字符长行结尾 `tos> XXXX`。字节不丢（长行 400 个 X = 200 回显
+ 200 输出），是顺序错。量化：整行突发输入 3/9 错乱，逐字符 5 ms 输入 0/6；
人手打字不触发，粘贴和协议输入会。

根因：`arch/rv32/platform.c` 的 UART RX 中断路径是 `tty_poll()` →
`plat_irq_service()` → `process_console_kick()` → 直接抢占，**没有
`console_echo_tick()`**；只有 tick（`process.c`）和 idle 排空 `echo_q`。
C3 的 `plat_console_tx_room()` 一 tick 最多放行 100 字节，shell 被中断立刻
唤醒并直接 `console_write` 提示符，队列里剩下的行尾要等下一个 1 ms tick。

修法：`console.c` 把排空抽成 `echo_drain_locked(max)`，tick 传发送器空闲量
（不阻塞），**每个同步输出入口**（`console_write` / `console_putc` /
`kprintf`）在写自己第一个字节前传 `-1` 抽干；RX 中断路径也补一次
`console_echo_tick()` 让回显尽早发。逐字节输出逻辑保持原样（不走 `out_char`，
否则「任务输出 → 回显」的所有权切换会插入 `\r\n`，把输入挤到下一行）。
顺手删掉从未被赋值的死变量 `in_echo`，并改正自相矛盾的顶部注释。
实测：突发 3/9 → **0/9**，行编辑（backspace / ^U / Tab / 方向键 / history）
逐字节正确。

### `priotest` 1 hog 判据稳定 FAIL —— 同一个根因

| 配置 | 基线 11-04 | 9dd076e | 修复后 |
| --- | --- | --- | --- |
| 1 hog NORMAL | 976 | 983~986 | 982~999 |
| 1 hog nice 0 | 990 | **948~952** | **1000** |
| 2/3 hogs nice 0 | 988 / 986 | 987~989 | **1000 / 1000** |

`priotest.py` 修前 3/3 复跑稳定 FAIL（`rt_bad > normal_bad`），修后 3/3 PASS，
且 0 级最差间隔从 2 ms 降到 1 ms。原因是积压的回显在 tick 里排空会占掉
1 ms 唤醒的相位，不是调度器改坏了 —— 调度器一行没动。

### 同时清掉的

- `process_exit()` 的 `[exit pid=N code=C cpu=0]` 调试打印删除，并把
  `if (!p) panic(...)` 移回解引用之前。
- `board/esp32c3/platform.c` 的 `#ifdef TOS_XIP_FLASH_BASE` 改成
  `#if TOS_XIP_FLASH_BASE`：兜底 `#define ... 0u` 让 `#ifdef` 恒真，
  全 RAM 构建也会跑 `xip_report()` 去读未映射的 `0x42000000`。改后重新构建，
  `esp32c3.bin` 与 ELF 的 SHA256 逐字节不变，XIP 路径行为未动。

### 真机验证（COM4，整片烧录）

5 次冷启动逐字节一致（593 B、无 panic）；`demo.sh` 全标记；
`py demo.py` sum:13 / fib(10):55；`pytest.py` 54/0、`pytest2.py` 21/0；
^C 三级目标全 `[pid N] killed`；crash/prot/deep/dd 写读校验/管道/重定向；
`transtest.py` 1024/16384/65536 三档 CRC 全过；`tosput-cli` 32 KiB CRC 过；
`rawstorm` 511 B / lost 0；`clocktest` b=996.65；9 目标 build、Go test/vet、
QEMU 三目标 smoke 与 compat 全过。**XIP 下擦写 flash 会自毁的担心撤回**：
`flash_irq_off()` 的 icache 热行 + 关 MIE + ROM 函数 + `.text.trap` 在 SRAM
是自洽设计，64 KiB 持续编程实测通过。

### 模拟器另开任务（不属本次改动）

`9dd076e` 的 XIP 启动桩调 ROM `0x400004d8`，rv32emu-c 只建模 `0x400004d4`，
`emutest` 因此挂掉。补 stub 后内核已能跑进 XIP 代码（`exceptions=0`），但仍
卡在启动早期（`mstatus MIE=0`、`cpu interrupts=0`、UART 零输出、无 prompt），
模拟器还缺一层建模；主仓 gitlink 也被该提交从 `6909215` 退回 `4e8cf59`。
这两件事独立推进，本次不提交 gitlink、不推子模块。

## 16. 用户 ELF 段头表裁剪（2026-11-06，已落地）与输出锁暂缓

### 输出锁：改动带回真实时间回归，本轮不落地

Review `9dd076e` 时发现 `console.c` 五处 `spin_trylock` 失败就整行/整次丢弃，
`process.c` 的 `FD_CONSOLE` 仍无条件返回 `len`。单核上 `spin_trylock()` 恒成功，
丢输出只在双核发生；而 ARM 的 `plat_spin_acquire()` 带 `SPIN_LIMIT`，无界自旋
确实挂过板（关中断等锁 / 持有者等本核的设备中断）。试图按「中断侧 trylock、
任务侧有界等待 + 返回真实字节数」分类修复，最小实现只 +48 B 内核代码，
**却在 C3 真机上让 `priotest` 1 hog 稳定 FAIL**：

| 镜像（同一块板，逐次整片烧） | 内核代码地址 | 1 hog normal / nice 0 | priotest |
| --- | --- | --- | --- |
| HEAD 原版 | `0x42010528` | 999~1000 / 1000 | PASS 2/2 |
| HEAD + **仅段头裁剪** | `0x42010528` | 999~1000 / 1000 | PASS 2/2 |
| 新 console + 未裁剪 ELF | `0x420105b0` | 989 / 986 | FAIL 2/2 |
| 新 console（最小版）+ 裁剪 | `0x42010558` | 988~989 / 986 | FAIL 2/2 |
| 新 console（完整版）+ 裁剪 | `0x420105b0` | 989~990 / 985~987 | FAIL 4/4 |

0/2/3 hogs 各档在新镜像里仍正常（3 hogs 的 nice 0 是 1000/1000），
`clocktest` b=996.59（console 实验镜像）与基线 996.65 一致，时钟没变；坏的只有 1 hog 那约 1% 的采样窗口。
`user/samp.c` 顶部注释本来就写着：「a tick landing between the read and the
call quietly costs a whole millisecond, about 1% of samples」。结论是
**XIP 内核代码布局一变，0 级采样器的相位窗口就翻面**，与段头裁剪无关
（表中第二行 PASS 是直接证据）。

本轮按「不带着回归上线」把 console / process 改动全部回退，只交付裁剪。
要落地这条得先解决布局敏感性：把新增代码放进排在热路径之后的独立 section，
或让 `samp` / `priotest` 的测量不再依赖启动相位，或用
`#if PLAT_MAX_CPUS > 1` 让单核产物与 HEAD 逐字节相同。另开任务跟进。

**2026-11-06 晚修正归因（§17 的反例）**：上面的 A/B 表是实测事实，但
「**只要 XIP 内核代码地址一变，相位窗口就翻面**」这个因果说过头了。§17 的
`process.c` 改动把内核代码从 `0x42010528` 搬到 `0x42010800`（**+728 B**，比表里
任何一版 console 改动都大），`priotest` 1/2/3 hogs 仍全部 PASS，1 hog nice 0
依旧 1000/1000。所以地址搬动本身不是充分条件；更合理的解释是 console 输出锁
动到了 tick / 中断收尾的**热路径时序**，而 §17 只改了 `exec` 这条冷路径。
结论仍然成立 —— 改内核后必须 HEAD vs 新镜像各整片烧做真机 A/B —— 但不能
拿「代码变大」预判结果，也不能用它替一个真实回归开脱。

### 段头表：程序头能触达之外的字节全部不要

`kernel/elf.c` 的一切都走程序头表：`header_ok()` 只校验 `e_phoff/e_phnum`，
`elf_probe()` 找 `PT_LOAD`，`elf_tos_note()` 找 `PT_NOTE`，
`apply_relocations()` 走 `PT_DYNAMIC` + `vaddr_to_off()`；**`e_shoff` 从未被读**。
`-s` 能去掉符号表，去不掉段头表本身，而它后面还挂着只为它存在的 `.shstrtab`。

`internal/build/TrimELFSections()` 接在 `linkPIE()` 之后：只处理「ELF32 小端、
段头表正好在文件尾、且没有任何程序头指向尾部」这一种形状，把
`e_shoff` / `e_shentsize` / `e_shnum` / `e_shstrndx` 四个字段清零，
文件截到「所有程序头 `p_offset+p_filesz` 的最大值」——`.shstrtab` 和填充
一起消失，产物仍是合法 ELF32（`debug/elf` 能打开、程序头完好）。
布局不符就原样保留：省几百字节不值得一个加载不了的程序。

实测（esp32c3 目标，36 个真正进镜像的程序，`TestL2Sizes` 同一口径）：

| | 原始 ELF | LZ4TOS 后 |
| --- | --- | --- |
| 裁剪前 | 123 404 B | 91 678 B |
| 裁剪后 | 111 316 B | 84 529 B |
| **省** | **12 088 B** | **7 149 B** |

`bin/py` 31 500 → 31 014 B，也就是 `exec` 的 `raw_size` 解压缓冲同步变小
（C3 内核堆只有 56 KiB）。真机 `df`：littlefs Used **233 472 → 221 184 B**，
可用多出 12 KiB。FS 镜像本身仍是固定 300 KiB，省下的是块空间而不是镜像文件体积。

代价：`build/user/bin/*` 不能再 `readelf` / `objdump -d`（`-s` 早已去掉符号，
实用价值有限）；内核 `build/kernel-*.elf` 完全不受影响，反汇编照旧。
新增 Go 测试 `TestTrimELFSections`（合成 ELF、幂等、三种「不该动」的布局）
与 `TestStagedProgramsHaveNoSections`（校验上次构建真正进镜像的产物）。

验证：9 目标 build、`go test -count=1 ./...` / `go vet` / `go build`、
QEMU `x86` / `rv32` / `arm` / `rv32smp` smoke 全 OK，`compat` COMPAT OK。
一个测试顺序陷阱：`smoke` 默认命令里有 `wfile demo.txt ...`，x86 的
`build/tinyos.img` 是持久磁盘镜像，跑完会留下 `demo.txt`；紧接着跑
`compat -no-build` 就会拿带这个文件的 x86 当基准，`ls` 假 FAIL。
让 compat 自己重建三目标（或先重建 x86）即可。

### 真机（COM4，整片重烧；镜像与构建产物 SHA256 逐字节一致）

3 次冷启动逐字节一致（各 593 B、无 panic、无 `[exit pid=]`）；
`df` 数字如上；`source etc/demo.sh` 全标记；`py demo.py` sum:13 / fib(10):55；
`pytest.py` 54/0、`pytest2.py` 21/0；`sleep 3000` + ^C `[pid 19] killed`；
crash / deep / `dd pattern verified` / 管道；回显突发 0/9、200 字符长行
400 个 X；**`priotest` PASS 2/2**（1/2/3 hogs 的 nice 0 全 1000/1000）；
`transtest` 3/3 CRC、`rawstorm` 511 B / lost 0、`clocktest` b=995.75
（交付态实测，与 A/B 阶段的 996.59 / 基线 996.65 同一水平）、`tosput` 32 KiB CRC 过。

## 17. LZ4TOS 信封预存窗口三量（2026-11-06 晚，已落地；B 路线第一步）

### 为什么需要它

「解压直接写进任务窗口」（§4 的 B 路线）要求窗口**先**分配、**再**解压，
于是窗口大小必须在解压之前已知。信封里本来就有 `original_size`（`mklfs.c`
的 `put_u32(hdr+8, flen)`，`kmalloc(raw_size)` 读的就是它），但它只是**文件字节数**，
而 `kernel/process.c` 算的是：

```
win = align_up(span + stack + heap_ask + HEAP_GUARD, PLAT_APP_GRAIN)
span = align_up(max(PT_LOAD.vaddr+memsz) - min(PT_LOAD.vaddr), PAGE)   /* 含 .bss */
stack / heap_ask = .note.tos 的 TOS_STACK() / TOS_HEAP()
```

实测 C3/RV32 的 36 个进镜像程序，**36/36 都是 window > raw**：
`py` 31 014 → 131 072 B（4.2×）、`mem` 1 318 → 81 920 B（**62.2×**）、
`dd` 2 486 → 32 768 B；raw 合计 110 184 B，window 合计 950 272 B。
拿 `original_size` 当窗口大小，`mem` 会被分到 1.3 KiB 而不是 80 KiB，加载时直接写越界。

不预存就得两阶段，而两阶段有两个硬伤：LZ4 的 match 回溯要求**输出连续**，
阶段 1「解出 ELF 头」也得有一块连续内存，可窗口还没分配 —— 又退回内核堆缓冲；
而且 `.note.tos` 在 `user.ld` 里排在 `.rodata` 之后，`py` 的 note 落在文件
**29 284 B** 处（31 014 B 的 94%），阶段 1 几乎解完整文件，再从头重解 ≈ 解两遍。
（§4 / 上一轮对话里「先流式解出头部」的说法低估了这个成本，此处更正。）

### 落地内容

`flags` bit0 标记扩展头，16 → **28 B**：`span` / `stack_ask` / `heap_ask` 三个 u32。
`tools/mklfs.c` 的 `elf_window_hint()` 按 `kernel/elf.c` 的同一套规则从 ELF 算出它们；
`kernel/lz4.c` 的 `lz4_wrap_info()` 改为返回 LZ4 块的偏移（0 = 不是信封）并填 hint；
`kernel/process.c` 把 `proc_alloc()` 提前，有 hint 时用本板常量现场算 `win` 并
`win_alloc()` **在解压之前**分配，解压后再用 `elf_probe()` / `elf_tos_note()` 复核
span / asked_stack / heap_ask / win 四项，不一致拒绝加载（`elf_load` 的窗口边界检查
是第二道防线）。`flags=0` 的旧 16 B 信封走原路径。栈请求校验抽成
`stack_request_ok()`，hint 与 note 共用同一套规则。

**为什么不直接存 `win_size`**：grain（C3 16 KiB / x86 64 KiB）和默认栈（8 / 32 KiB）
是目标相关的，存结果会把镜像绑死到一块板 —— 而选 ELF32 PIE 的初衷正是「同一份
二进制跨板分发」。存三个 ELF 派生量、由内核补上自己的常量，两头都保住。
`mklfs -v` 现在会打印 `hint=span/stack/heap`，便于只读诊断。

### 验证（全部实测）

| 项 | 结果 |
| --- | --- |
| 离线 | 9 目标 build 全 rc=0；`go test -count=1 ./...` / `go vet` / `go build` rc=0；QEMU x86/rv32/arm/rv32smp smoke 全 `SMOKE TEST OK`；`compat` 自带 rebuild 后 `COMPAT OK` |
| 新测试 | `lz4hint_test.go`：用 `debug/elf` 独立算参考值，经 `mklfs` 打包 + `-v` 读回，**36 个 hint 逐一一致**；不可解析的假 ELF 正确退回旧 16 B 头 |
| 真机 A（板上原 HEAD 镜像） | `priotest` 1 hog：normal worst 2 ms，nice 0 **1000/1000** worst 1 ms，PASS；`free` peak 28 408 B |
| 真机 B（整片重烧） | `priotest` 1/2/3 hogs **全 PASS**，1 hog nice 0 重复两次仍 1000/1000；`accept.py` **24/24**；`py demo.py` sum:13 / fib(10):55、pytest 54/0、pytest2 21/0、`^C [pid 15] killed`；`transtest` 3/3 CRC、`rawstorm` 511 B、`clocktest` b=995.7731、`tosput` 32 KiB CRC `735de6d1` |
| 冷启动 | 3 次各 593 B、逐字节一致、无 panic；板上镜像 `de76bb4c…` / `48da9800…` 与构建产物一致 |
| 旧格式兼容（**已作废**） | 当时只烧旧 16 B 头 FS、保留新内核：shell 正常起、`hello`、`py demo.py` 全过。§18 之后不再兼容旧信封，这条只留作历史 |

**必须整片烧**：新 FS 是 28 B 头，旧内核的 `lz4_wrap_info()` 遇到 `flags != 0`
会拒绝信封、把它当 plain ELF，所有程序都加载不了；反过来（新内核 + 旧 FS）兼容。

### 收益与代价（诚实口径）

- 内核代码 `0x42010528 → 0x42010800`（**+728 B**），`esp32c3.bin` 329 000 → 329 728 B。
- 36 × 12 B 的头增长**没有多占 flash**：`df` Used 仍是 221 184 B，432 B 被
  littlefs 现有块内空隙吸收了。
- `free` peak 28 408 → **28 424 B**（+16 B，压缩件多 12 B 加块头对齐）。
- **堆峰值没有下降**，这步只把窗口分配提前、把数据通道和复核机制建好；
  解压缓冲仍在内核堆。真正把 56 KiB 堆压到几 KiB 由 **§18** 完成
  （堆 56 → 16 KiB、峰值 28 424 → 768 B）。
- 另外两个独立障碍：LZ4 输出必须连续（任务窗口正好满足）；`p_offset != p_vaddr`
  （`py` 448 / `sh` 384），线性 ELF 要一次 `memmove` 摆正并先备份 phdr、调整 rela_off。

## 18. 流式解压直写任务窗口（2026-11-06 深夜，已落地；B 路线第二步）

§17 铺好的窗口预分配到这里才真正兑现货：`exec` 不再把镜像搬进内核堆，
解压输出直接落进任务窗口。**内核堆从此与程序大小无关。**

### 落地内容

| 文件 | 改动 |
| --- | --- |
| `kernel/lz4.c` | 新增 `lz4_stream_decompress()`：输入侧 256 B 环形缓冲 + 回调 refill，token、255 扩展长度、2 字节 match offset 都可跨块。**删除**整块解码器 `lz4_decompress_block()` |
| `kernel/lz4.h` | `lz4_wrap_info_prefix()` 只读前 28 B + 文件总大小即可判型；信封为唯一格式，`flags` 必须恰为 bit0 |
| `kernel/ramdisk.c` | **删除** `find_locked()` / `ramdisk_find()` / `ramdisk_release()`（整份 `kmalloc` + 循环读）。顺序读复用已有的 `ramdisk_open/read/seek/close`（8 句柄 × 512 B littlefs 缓存） |
| `kernel/elf.c` | **删除**线性视图，只留窗口视图：`view_at()` 经 `PT_LOAD` 把「文件偏移」换算成「窗口地址」；程序头副本在滑动前拷出，再逐段 `memmove` 到 `p_vaddr - link_lo` |
| `kernel/process.c` | `exec` 单一路径：读前缀 → `proc_alloc` → 用本板常量算 `win` → `win_alloc` → 流式解压直写 `win_base` → 逐段摆正 → `elf_load_windowed()` → 复核三量与 ELF 一致 |
| `tools/mklfs.c` | 不再写 16 B 头：算不出 hint 的「ELF」直接报错打包失败，而不是产出一个内核加载不了的文件 |

`win_need` 用 `max(span, raw_size) + stack + heap_ask + guard`：解压先要能装下
**整份线性 ELF**，摆正之后才是 `span` 那份布局。现有 36 个程序都是 `span` 更大，
但 64 KiB 级程序在小板上可能是 `raw_size` 更大，所以两个都算进去。

### 验证（全部实测）

| 项 | 结果 |
| --- | --- |
| 宿主交叉验证 | `tmp/lz4stream/harness.c` 用 `tools/lz4.c` 压缩 36 个真实 ELF，chunk = 1/2/3/5/7/16/64/127/256/4096 各解一遍，加 short-cap 与截断用例，**432 例全过**，输出逐字节一致 |
| 离线 | 9 目标 build 全 rc=0；`go test -count=1 ./...` / `go vet` / `go build` rc=0；QEMU x86/rv32/arm/rv32smp smoke 全 OK；`compat` `COMPAT OK` |
| C3 真机功能 | `accept.py` **24/24**；`py demo.py` sum:13 / fib(10):55、pytest 54/0、pytest2 21/0；`source etc/demo.sh` 全标记；`^C [pid 15] killed`；管道、`dd pattern verified`；无 `hint disagrees` / `corrupt` / panic |
| C3 真机压力 | `transtest` 3/3 CRC；`rawstorm` 511 B / lost 0；`clocktest` b=995.7731；`tosput` 32 KiB CRC 过 |
| 冷启动 | 3 次各 593 B、逐字节一致 |

### 堆峰值：这才是真正的收益

| | 改前（§17 交付态） | 改后 |
| --- | --- | --- |
| `free` peak（只跑过 shell） | 28 424 B | **768 B** |
| `free` peak（跑过 `py demo.py`） | 54 752 B | **1 024 B** |
| C3 内核堆 | 56 KiB | **16 KiB** |
| C3 内核 SRAM（`kernel RAM ends`） | 120 048 B | **79 088 B** |
| C3 内核窗口余量 | 12 832 B | 52 KiB |

峰值不再是「最大那个程序」，而就是 littlefs 的文件缓存。程序大小的天花板因此
从**内核堆**换成了**任务窗口**（C3 240 KiB 用户区），64 KiB 以上的程序第一次有可能。

### `priotest` 1 hog 的 A/B（重要方法学）

新内核 1 hog 出现 FAIL，先按纪律扩样本，再做真机 A/B：

| 镜像 | 1 hog 样本 | PASS |
| --- | --- | --- |
| HEAD（`de76bb4c…`） | 20 | **16** |
| 流式加载 + 56 KiB 堆 | 9 | 6 |
| 流式加载 + 16 KiB 堆 | 20 | **17** |

**这个判据本身就有约 20% 的偶发 FAIL**，与堆大小、加载路径无关。结论：无相位回归。
（这也修正了 §16 的过度归因——不是「改内核就翻相位」，而是这条判据接近临界值，
任何布局变动都可能把它推过线。判断回归必须连跑 3 次以上、样本 ≥ 10。）

### 代价

- 内核 `.text` 因流式状态机与窗口视图变长（C3 上 ±数百字节量级），XIP 代码地址搬动。
- **必须整片烧**：新内核拒绝 16 B 信封，旧 FS 上所有程序都加载不了
  （用户明确表示不需要兼容旧格式）。
- 窗口里要多留「线性 raw ELF」的空间，`win_need` 因此取 `max(span, raw_size)`。

## 19. RP2040 的 RAM 到底被谁吃了（2026-11-06 实测盘点，未改动）

Pico 只有 **264 KiB** SRAM，比 C3 的 400 KiB 还紧，值得单独盘一次。以下全部是
`build/kernel-rp2040.elf` 与源码常量的实测/实算值，不是估计。

### 现状：内核只用了一半自己占的地

| 项 | 字节 | 说明 |
| --- | --- | --- |
| `.text` | 72,372 | **在 XIP flash（0x10000100），不占 SRAM** |
| `.data` | 28 | |
| `.bss` | 77,536 | 见下表 |
| `__kernel_end` | **77,568** | `0x20012f00`，链接脚本 ASSERT 卡在 `0x20020000` |
| 内核窗口（`0x20000000..0x20020000`） | 131,072 | 128 KiB |
| **窗口内闲置** | **53,504** | 因为 `PLAT_KERNEL_MAX` 写死 128 KiB |
| 用户区 | 136 KiB | `0x20020000..0x20042000`，grain 16 KiB → **8 个窗口** |

### `.bss` 明细（77,536 B）

| 项 | 字节 | 可调？ |
| --- | --- | --- |
| 内核堆 arena（默认 32 KiB） | 32,768 | ✅ `HeapBytes` |
| 内核栈（`ARM_KSTACK_BYTES`） | 16,384 | ✅ 需与 `kernel/cpu.h` 同步 |
| 任务表 `procs[16]`，`sizeof(proc_t)=728` | 11,648 | ✅ `MaxProcs` |
| littlefs 整块 bounce（4 KiB 块） | 4,096 | ⚠️ 拆开可省 3.5 KiB，但每次整块操作要拆 8 次 |
| tty：行缓冲 1024 + 历史 512 + 行 256 + 回滚 256 + raw 环 512 | 2,560 | 部分可关（`TOS_HIST_BYTES=0`） |
| 目录快照 20×32 + 名字缓冲 | 736 | |
| littlefs 3 个 512 B 缓存（read/prog/look） | 1,536 | 否 |
| 文件句柄池 `openfiles[8]`，`sizeof(ofile_t)=172` | 1,376 | 可降到 4 |
| 屏幕网格 20×48 | 960 | ✅ rp2040 没有 LCD，可条件编译掉 |
| 引脚表 48×2×8 | 768 | 可降到 16 |
| devfs 名字表 32×16 | 512 | 可降到 16 设备 |
| 控制台回显队列 | 256 | |
| `lfs_t` + `lfs_config` | 212 | |
| 其余零散 + 对齐 | ≈3,700 | |

`sizeof(proc_t)=728` / `sizeof(ofile_t)=172` 是 `clang -Xclang -fdump-record-layouts`
对 armv6m 目标实测的，不是手算。`proc_t` 里最大的是 `cwd[64]`、`regs_t saved`
（80 B）和 `file_t fds[8]`。

### 实测：两个旋钮能省多少

把 rp2040 临时改成 `HeapBytes: 8*1024, MaxProcs: 8` 重建（已还原，未提交）：

```
kernel ends at 0x20012f00   (77,568 B)   →   0x2000b840   (47,168 B)
```

**−30,396 B**，与「24,576（堆）+ 5,824（任务表）」逐字节吻合。也就是说这两个
数字不是纸面推算。

### 建议的动手顺序（收益大 → 风险高）

1. **先把白占的 50 KiB 让出来**（零代码）：`PLAT_KERNEL_MAX` 从 `0x20020000` 下移到
   `0x20014000`、同步改 `board/rp2040/kernel.ld` 的 ASSERT。用户区 136 → **184 KiB**
   （8 → **11 个窗口**），内核仍有 4.3 KiB 余量。这一步不碰任何 C 代码。
2. **堆 32 → 8 KiB**（`HeapBytes`）：−24 KiB。C3 上 16 KiB 已过全部 flash 压力测试，
   8 KiB 是新的未知，必须重跑 `tosput` / `transtest` / 大目录 `ls`。
3. **任务表 16 → 8**（`MaxProcs`）：−5.8 KiB。rp2040 无论如何最多只有 8 个窗口。
4. **内核栈 16 → 8 KiB**：−8 KiB。`ARM_KSTACK_BYTES`（`arch/arm/start.S`）与
   `KSTACK_BYTES`（`kernel/cpu.h`）必须同时改，并且用栈底哨兵在真板实测高水位。
   风险最高的一项，放最后。
5. 做完 2/3/4 后 `__kernel_end` ≈ 39 KiB，可把 `PLAT_KERNEL_MAX` 再下移到
   `0x2000C000`：用户区 **216 KiB / 13 个窗口**（+80 KiB，+59%）。

板级裁剪（屏幕 960 + 历史 512 + raw 环 256）是最后 1.7 KiB，优先级最低。

### 不要做的

- **拆 4 KiB bounce**：littlefs 可能一次要整块，省 3.5 KiB 换每块 8 次扇区循环，
  在 SRAM 最紧的板上也许值，但要先量 littlefs 实际请求分布。
- **只读 littlefs / 自研 FS**：早前已否决（掉电正确性验证成本过高，17.6 KiB 是必要成本）。
- **压缩内核 .bss / swap**：没有 MMU、没有 swap 介质语义，做到最后是自研换页，
  与本项目「宁可牺牲功能也不牺牲体积」的取舍冲突。

### 为什么现在没动

rp2040 手边没有可烧的 Pico，而项目纪律是**内核改动必须真板 A/B**（§16、§18）。
上面第 1 步是纯布局、可离线验证；2/3/4 需要真板。等有板子再按顺序做。

## 20. C3 极限裁剪：把 §19 的动作先在能烧的板上跑一遍（2026-11-07，已落地并真机验证）

### 动机

§19 把 RP2040 的账算清了，但那四条削减一条都没验证过，因为手边没有 Pico。
C3 有板子、有串口、有 24 项验收套件，而且它同样"内核窗口写得比实际用的大"。
所以先在 C3 上把同一类动作做到底：**这不是给 C3 救火（它有 370 KiB），
是给 RP2040 做预演**——凡是能在这块板上暴露的错，就不要留到 Pico 上再踩。

### 三项削减（全部通过 `internal/build/build.go` 与 `kernel/cpu.h`）

| 项 | 之前 | 之后 | 省下 |
|---|---|---|---|
| 内核栈 `KSTACK_BYTES`（RV32） | 32 KiB | **16 KiB** | 16 384 |
| 内核堆 `HeapBytes` | 16 KiB | **8 KiB** | 8 192 |
| 进程表 `MaxProcs` | 14 | **8** | 4 752（792 B × 6 槽） |
| 合计 | 79 088 B | **49 760 B** | **29 328 B** |

`__kernel_end` 从 `0x403934f0` 落到 **`0x4038c260`**（逐字节吻合）。

RV32 内核栈减半的依据不是"猜"：ARM 端口从存在起就是 16 KiB（RP2350 整个内核只有
128 KiB），而两边跑的是同一套内核代码，内核态最深路径是"trap 帧 + 一次 syscall"，
`py` 的递归发生在**用户栈**上。所以 RV32 那 32 KiB 里的一半从来没被碰过。
`arch/rv32/start.S` 的 `KSTACK_SHIFT` 15 → 14，并且 `.space` 改成由同一个宏计算
（原来写死 `32768 * TOS_MAX_CPUS`，与 `KSTACK_SHIFT` 是两个独立的真相源，
i386 上就是这样漂移出了幽灵进程）。

`cpu.h` 的 `#if` 从 `arm` 扩到 `arm || riscv`，注释同步改写成"两个端口各自为什么
要 16 KiB"，x86 保持 32 KiB（`arch/x86/entry.S` 就是 `.space 32768`）。

### 第四项：布局下移（最大的一块）

`PLAT_KERNEL_MAX` 原本写 `0x403A0000`（128 KiB 预算），内核实际只用到
`0x4038c260`，中间 **50.8 KiB 是死区**：内核长不到（它用不了那么多），
窗口也拿不到（任务区从 `PLAT_KERNEL_MAX` 起算）。

改成 **`0x40390000`**（64 KiB = 4 grains，任务区能落脚的边界）：

- 余量 `0x40390000 - 0x4038c260` = **15 776 B**（超出就链接期 ASSERT，不会静默踩 pid 1）
- 任务区 `0x40390000..0x403dc000` = **304 KiB = 19 × 16 KiB**（原 240 KiB / 15）
- `free` 实测：`kernel 49 760 / tasks 311 296 / unused 17 584`
  （unused = 15 776 内核余量 + 1 808 尾部 ROM 保留）

同步改了三处引用：`board/esp32c3/kernel-xip.ld` 与 `kernel.ld` 的 ASSERT，
以及 `kernel/platform.h` 里 NAPOT 对齐那段注释里的地址。

### 真机结果（COM4，整片烧）

| 项 | 结果 |
|---|---|
| 启动 | `kernel heap: 8 KiB arena`、`tick self-check: 2`、shell 起 |
| `accept.py` | **24/24 PASS** |
| `py demo.py` / `pytest.py` / `pytest2.py` | 全过，54 + 21 项 checks 0 failed |
| 堆峰值 | **1 024 B**（跑完 py 之后），8 KiB 有 8 倍余量 |
| 进程表 | shell + 7 个后台 `sleep` = 8 满；第 9 个报 `exec: process table full` |
| `transtest` 1 KiB / 4 KiB | 2/2 CRC 通过 |
| `clocktest` | `b = 991.06`（1000 为完美；上次是 995.77，属测量抖动） |
| `priotest` 1 hog | 8 次中 7 PASS（符合该判据 ~20% 偶发 FAIL 的历史水位） |
| `priotest` 1/2/3 hogs | 3 次中 2 次全 PASS，1 次单点 FAIL |
| 冷启动 ×3 | 237 B 启动日志 SHA 三次一致 |
| 9 目标构建 | 全过（rv32 / rv32smp / rv32nc 也吃到 16 KiB 栈） |
| QEMU `smoke -target rv32` | **SMOKE TEST OK** |

产物：`build/esp32c3.bin` = `c0a117f1…`，`build/ramdisk-esp32c3.bin` = `48da9800…`
（后者未变，因为用户程序没动）。

### 对 RP2040 的映射（这是本次真正的产出）

| §19 的步骤 | C3 上是否已验证 | 说明 |
|---|---|---|
| ① 下移 `PLAT_KERNEL_MAX` | ✅ 已做 | RP2040 上同样能离线做，收益 136 → 184 KiB |
| ② 堆 32 → 8 KiB | ✅ 已做 | 同一套流式加载，堆峰值 1 KiB 级 |
| ③ `MaxProcs` 16 → 8 | ✅ 已做 | 满了报 `process table full`，不崩 |
| ④ 内核栈 16 → 8 KiB | ❌ 没做 | C3 只降到 16 KiB（ARM 本来就 16，没法用它验证 8） |

所以 C3 这一轮能替 RP2040 挡掉 ①②③ 的所有"配置写错、栈池漂移、进程表越界"
这类错，**唯独挡不掉 ARM 侧的 8 KiB 内核栈**——那必须在 Pico 上量（或者接受
"ARM 16 KiB 里也有一半空着"这个同样的论证，但项目纪律要求真板）。

### 不要做的（C3 上）

- **再减堆到 4 KiB**：峰值 1 024 B 看着够，但 littlefs 的目录遍历深度取决于
  文件系统内容，不是固定值；8 KiB 是 8 倍余量，再砍只省半个 grain（凑不出窗口）。
- **减 `bounce`（4 KiB）**：见 §19 的同一条否决。
- **`MaxProcs` 降到 6**：省 1.6 KiB，但少一个槽就少一个后台任务，
  而 8 已经是"shell + 前台子进程 + 2 后台"的舒适下界。

## 21. 一个 16 KiB 窗口的 shell：做得到（2026-11-07，原型已上板）

问题：`bin/sh` 现在要 **32 KiB 窗口**（镜像 20 464 + 栈 8 192 + 守卫 4 096 = 32 752，
离 32 KiB 只剩 16 字节）。能不能把脚本解释器摘出去，只留 `exec`/`source` 这种
启动脚本必需的东西，压进**一个** 16 KiB 窗口？

### 窗口预算是硬的

    16 384 = 镜像 + 栈 + 守卫(4 KiB)  →  镜像 ≤ 8 192

栈和守卫都有下限（一个页，而且要能用一个 NAPOT/MPU region 描述），
所以 8 KiB 是 `TOS_STACK(4K)` 之后的镜像预算。`user/msh/` 就是按这个预算写的原型。

| | text | rodata | bss | span | 余量 | 窗口 |
|---|---|---|---|---|---|---|
| `bin/sh`（-O2） | 10 766 | 4 623 | 5 008 | 20 464 | — | 32 KiB |
| `bin/sh`（-Oz） | 7 924 | 4 537 | 5 008 | 17 552 | — | 32 KiB |
| `bin/msh`（-O2, rv32，无 history） | 2 696 | 331 | 2 656 | 6 016 | 2 176 | 16 KiB |
| `bin/msh`（-O2, rv32，有 history） | 2 952 | 331 | 2 656 | 6 272 | 1 920 | 16 KiB |
| `bin/msh`（-O2, rp2040） | — | — | — | 7 968 | **224** | 16 KiB |
| **`bin/msh`（-Oz, rv32，现状）** | 2 494 | 348 | 2 656 | **5 792** | **2 400** | **16 KiB** |
| **`bin/msh`（-Oz, rp2040，现状）** | — | — | — | **6 656** | **1 536** | **16 KiB** |

`history` 内建精确地花掉 **256 B text、0 B bss**（rv32 6 016 → 6 272）。

### 224 B 余量是这次真正的发现

加完 history，rp2040 只剩 224 B（7 968 / 8 192）。这数字看着"还进得去"，但它
**不是稳态**：`bin/sh` 的 32 KiB 就只剩过 16 B（§21 开头），这是同一类陷阱。

它的失效方式还特别安静——**没有任何构建期检查**。`build.go` 只断言内核不越
`PLAT_KERNEL_MAX`，对用户程序的 `span` 是否超过窗口一个字都不查。超了会怎样：
`win_alloc()` 按 `span + 4 096 + 4 096` 对齐到 16 KiB 粒度，于是 msh 悄悄变成
**32 KiB 窗口**；在 rp2040 上那意味着 8 个窗口变 7 个，程序照跑，只是多吃一个
grain。更糟的组合是只剩一个空闲 grain 时——`exec` 直接失败，msh 在 Pico 上起不来。

解法不需要改一行代码，用项目已有的机制：`user/msh/opt` 里写 `-Oz`（`py` 同款）。
注意 **`opt` 只对目录型程序生效**——`build.go` 查的是 `user/<dir>/opt`，单文件的
`user/<x>.c` 走另一条循环，永远读不到 opt（第一次把 opt 放在 `user/msh/opt`
而源码还是 `user/msh.c`，日志里只有 `OPT py -Oz`，一个字的提示都没有）。所以
msh 改成了目录型，`user/msh/msh.c` + `user/msh/opt`。

| 目标 | -O2 span / 余量 | -Oz span / 余量 |
|---|---|---|
| rv32 (C3) | 6 272 / 1 920 | **5 792 / 2 400** |
| x86 | 7 792 / 400 | **6 640 / 1 552** |
| rp2350 | 7 680 / 512 | **6 208 / 1 984** |
| rp2040 | 7 968 / **224** | **6 656 / 1 536** |

x86 的 400 B 也一并解决了。换优化级别的代价是一个 shell 逐字符搬运、fork 程序，
没有可测的循环；真机上 `stk` 反而从 1.6 降到 1.3 KiB。

真机（C3，`ps`）：`2  1  1  wait  -  0.4  0  16  1.3/4  msh` —— 窗口 16 KiB、
栈高水位 1.3 KiB / 申请 4 KiB，跑的是嵌套 `source` 这条最深路径。

### 保留 / 砍掉

保留：读行、分词、`name=value`、`$name`/`${name}`/`$?`、`#` 注释、`&`、
`exec` 程序、`source`（同进程、可嵌套 2 层）、`exit`、`run`、`echo`、`true/false`、
`history`（列出）+ ↑/↓ 召回。

砍掉：`if/while/else/fi/done/break` + `eval_cond` + `scan_block`、`$(...)`、
管道、重定向、`incr`、`cd/pwd/rm/ps/prio/nice`、`sleep` 内建、长 help。
这些正是 `sh.c` 里 text 8 KiB / rodata 4.3 KiB 的来源。

### history 不用在这 8 KiB 里出钱

历史池不在 shell 里，在行规程（`kernel/tty.c`，512 B 内核 `.bss`，全平台同一个
尺寸），这也是 ↑/↓ 能用的原因 —— 它连 shell 是哪个都不知道，只认 `read()`
的字节流。于是：

- **召回（↑/↓、^W、退格、Delete）本来就是免费的**：msh 用 lib 的 `getline`
  走 cooked 路径，箭头在 tty 就被吃掉了，`read()` 返回的是重写过的整行。
  实测把手里的 msh（改动前那一版）启动，敲 `echo marker-abc`、按 ↑、回车，
  再次输出 `marker-abc`。
- **`history` 内建只是那个池的读者**：`hist_count()` / `hist_get()` 两个
  syscall（`SYS_HIST`），256 B 的复制缓冲放在栈上 —— 窗口已经为 4 KiB 栈付过钱，
  嵌套 `source` 的高水位才 1.6 KiB，这 256 B 不动 `.bss`。
- 所以"给 history 留 256 B"这个预算，花在了**代码**上（右对齐 + 循环），
  存储一个字都没花。

真机（C3）：`history` 打出 6 行右对齐列表（含 `run msh` 这条由完整 sh 敲进去的
命令 —— 池与 shell 无关，谁敲的都记），↑ 两次退格重绘回 `ls bin` 后回车正常执行。

**rp2040 的 224 B 已经被 `-Oz` 换成了 1 536 B**（见上一节），所以 pool 不必再动。
真要再挤，两条仍然在桌上：pool 2 KiB → 1 KiB（`etc/rc` 941 B 够，`etc/demo.sh`
1 140 B 装不下了），或者反过来把内核 `TOS_HIST_BYTES` 从 512 降到 256，省 256 B
内核 `.bss` —— 代价是 `HIST_KEEP_MAX`（召回时暂存正在编辑的那一行）从 256 掉到
64，且池只能留住约五六条短命令。C3 / rp2040 上都没必要。

### 五个坑（都踩过）

1. **裸 ELF 不能 `exec`**：上传到板子上的文件不会经过 `mklfs`，没有 LZ4TOS 信封，
   `exec` 直接报 "not found or not a loadable program"（§17 之后不再有 plain-ELF 路径）。
   验证一个用户程序必须把它编进镜像。
2. **pool 必须装得下 `etc/rc`**：rc 941 B，所以 1 KiB 的池只剩 81 B 余量，
   `etc/demo.sh`（1 140 B）就进不去了。原型用 2 KiB。
3. **全注释脚本的返回码是 0**：`run_script` 的 `code` 从 0 起，不能跟
   `run_command` 的 -1（继续执行）混用 —— 第一版把 0 行的 rc 报成了 "cannot open"。
4. **`opt` 只对目录型程序生效**：`build.go` 查的是 `user/<dir>/opt`，单文件的
   `user/<x>.c` 走另一条循环，读不到。第一次把 opt 放在 `user/msh/opt` 而源码还是
   `user/msh.c`，构建日志里只有 `OPT py -Oz`，其余一片安静 —— 尺寸一个字节没变。
   这也解释了为什么 msh 最后是目录型。
5. **用户程序没有任何尺寸检查**：`build.go` 只断言内核不越 `PLAT_KERNEL_MAX`。
   程序超出 8 KiB 预算不会报错，`win_alloc()` 直接对齐到 32 KiB 窗口。在只有一个
   空闲 grain 的板上，症状是 msh 起不来而不是构建失败。

### 待拍板

`bin/msh` 目前只是**多一个程序**，`bin/sh` 没动，内核仍然 `process_start_shell("bin/sh")`。
两条路：

- **A. 换名**：`bin/sh` = 最小（init），完整版改名（如 `bin/shb`）留给带块语句的脚本。
  省一个 grain，但 `source etc/demo.sh`、`run sh <file>` 这些既有用法和文档全要跟着改。
- **B. 并存**：`bin/sh` 不动，小板上把 init 名字指到 `bin/msh`（要加一个板级常量），
  FS 里两个 shell 都在，多约 2 KiB flash。

## 22. RP2350 双核实测：`getcpu()` 偶尔返回系统调用号（2026-11-08，未定性）

目标：用户明确要求「RP2350 也插着，做一下测试，貌似 SMP 还有一定问题」。
测试台：`-target rp2350smp`（2 核）、tick 1000 Hz、UART0 @ COM6 115200、
`user/smp.c` 的 `-s <n> <ms>` 单行汇总模式。Ctrl-D（0x04）写到 COM6 让固件自己
跳 ROM BOOTSEL，全程无需按键。

### 稳定复现的三件事

1. **`smp -s 4 500` 的退出码偶尔是栈指针**，且值固定落在某个任务初始栈附近：
   `537116152` = `0x2003BDF8`、`537181688` = `0x2004BE78`、
   `537214456` = `0x20053DF8`、`537148920` = `0x20043DF8`。
2. **`getcpu()` 偶尔返回 25** —— 而 `SYS_GETCPU` 的编号正好是 25
   （`include/tos.h`）。给 worker 加自检后拿到直接证据：
   `worker 1: BAD c=25 x2 (nr would be 25)`（500 ms 内约 2000 次调用中 1~2 次）。
   **返回的是系统调用号本身，说明这次 SVC 的 `r0` 回写没有落到用户帧上。**
3. **4 个可运行任务在 2 核上严重不公平**：同为 500 ms，块数
   `27 / 62 / 248 / 323` 对 `2500 ~ 5000`，差 9~100 倍；`smp -s 2` 反而正常
   （`5915 / 6123`）。同一 summary 里还会出现 `w1=-1`（`wait()` 找不到孩子）。

### 已排除（都带判据）

| 假设 | 判据 | 结果 |
|---|---|---|
| `make_ready()` 重复唤醒 READY/RUNNING 的任务 | 内核计数器 `wbad` | 0 |
| `block()` 被非 RUNNING 的任务调用（已在双核上跑） | `bbad` | 0 |
| 子进程退出时父进程还在 RUNNING（丢唤醒） | `rrun` | 0（个别 1） |
| 恢复的帧属于别的任务 | `bpc`：`saved.pc` 不在自己窗口内 | 0 |
| 恢复的 r0~r3 指向别的任务窗口 | `breg` | 0 |
| `plat_cpu_id()` 返回 25 | `SIO_CPUID` = `0xd0000000 + 0x000`，每核常量 | 不可能 |
| MSP 落到另一个核的内核栈 | `arm_svc` 的 pending 启动路径**故意不复制** `FR_TSP`，所以新任务的 MSP 天然是当前核的 | 否 |
| MPU 区域过宽导致串窗 | PMSAv8 base/limit 形式，32 B 粒度、精确 | 否 |

第二轮（用户指定「挖 r0 不回写」）又排掉三条，都是内核计数器，不靠打印：

| 假设 | 判据 | 结果 |
|---|---|---|
| `arm_resume` 走到「没有硬件帧地址就跳过写回」的分支 | 在 `arm_resume` 里给该分支加计数器 `arm_nohw_count` | **0**，一次都没走。写回总是发生 |
| 某次 SVC 被 `arm_svc` 的 pending-start 分支劫持（残留 pending 字） | `arm_pending_hits` 对比 `plat_enter_user_calls` | **2 == 2**，一次启动一次命中，没有残留 |
| `SYS_GETCPU` 的 case 没跑到（`sc.nr` 被读错） | 内核数 `probe_getcpu_calls`，与用户侧 `calls` 对账 | 某一轮增量 **7952 == 7952**，完全相等 |
| `plat_cpu_id()` 真的返回了 25 | 让 case 把 `id == 25` 换成 `99` 再返回 | 用户始终看到 25，**从没看到 99** |

把 `SYS_GETCPU` 的返回改写成哨兵这一条最能说明问题：如果内核返回的是 25，用户会看到
99；用户看到的仍是 25，说明那个值根本不是内核写进去的。而用户侧代码生成也查过了
（`clang --target=thumbv8m.main-none-eabi -Oz -S`）：`movs r0,#25 / svc #0 / mov r1,r0`
紧挨着，编译器没有把读取提到 svc 之前。

### 现在的证据链（互相矛盾，所以还没结案）

1. 用户每次 `getcpu()` 都对应一次内核 `SYS_GETCPU`（计数完全对得上）；
2. 该 case 每次都执行了 `plat_frame_set_ret(r, id)`；
3. `arm_resume` 每次都把 `frame->r0` 写进硬件帧（`nohw == 0`）；
4. 用户代码是把 `svc` 之后的 `r0` 直接搬走（汇编已确认）；
5. 可用户仍有约 1/2000 的概率读到 **25 = `SYS_GETCPU` 本身**，即「svc 之前的 r0」；
6. 出问题的下一次调用立刻正确（`next=0/1`），说明机制本身没坏；
7. 频率约每核每 1000 个 tick 一次 —— 与「一个 tick 落在 syscall 在飞的窗口里」这个
   概率同量级。

7 指向「tick 与 SVC 撞车」，但 1~4 又把所有已知的撞车路径堵死了。下一步最值得做的是
**让 `SYS_GETCPU` 同时写 r0 和一个 r4~r11 里的寄存器**（例如 `r->r9 = 0xAB00|id`），
用户侧用一小段内联汇编读回：如果 r9 是标记而 r0 是 25，说明帧确实被恢复了、只有 r0
是旧的（那问题在写回与读出之间的某个拷贝）；如果 r9 也不是标记，说明恢复的不是这一帧，
要回去查 `syscall_dispatch()` 返回值到 `arm_resume` 之间那两步。

### 顺带挖出的两个真 bug（与上面的谜团无关）

- **`exec()` 先发布后挂父**：`process_exec()` 里
  `proc_t *p = proc_create(...)` 已经把子任务写成 `ST_READY`（另核可立刻取走并运行），
  之后才 `p->parent = current`。子任务在那一瞬退出，`reap_into_parent()` 看到
  `parent == NULL` 直接丢退出码；更糟的是槽位已被回收复用，这一行会写到**别人的**
  `parent` 上。修法：把 `p->parent = current` 挪进 `proc_create_claim()` 的发布临界区
  （那里已经在用 `current` 拿 cwd/fds/prio）。
- **`wait(pid)` 的 `laggards`/`exit_code` 是单槽**：几个孩子先于父进程 `wait()` 退出时，
  退出码全部塌成最后一个，`wait(pid)` 拿到的码与 pid 对不上。实测
  `w1=w2=w3=27`、`w1=w2=w3=2188`、`w2=w3=2756`。**`smp -s` 的汇总在这一条修好之前
  不能当判据** —— 第一轮把 `27 blocks` 读成「worker 被饿死」是对的，但把
  `w1=w2=w3` 读成三个都饿死就错了。

### 方法论教训

第一轮我用 kprintf 做探针，得出「`make_ready()`/`block()` 都干净」的结论 —— **不可信**。
控制台在竞争下会丢整行（§16 的 `#78`），丢行与「没发生」无法区分：同一轮里
pid 18/19/20 的 `[E]` 行全部丢失，我因此误判「worker 1 从没退出」。
改成**内核计数器 + 空闲看门狗一次性输出**（`[Z dbl=.. wbad=.. wsram=.. bbad=.. rrun=.. bpc=.. breg=..]`）
之后结论才稳定。代价是 `dbl`（`current` 在 A 核、`cpu` 字段写 B 核）本身就是误报：
`sched_switch()` 走 idle 分支时**故意不清 `current`**，任务随后被另一核取走就满足该条件，
所以 `dbl` 只能当噪声看，不能当双跑的判据。

### 下一步（未做）

1. 定性 `r0` 不回写：把 `SYS_GETCPU` 的返回值同时写进 r0 和一个 r4~r11 寄存器
   （如 `r->r9 = 0xAB00 | id`），用户侧用内联汇编读回核对（理由见上）。
2. 4 任务 2 核的不公平要单独查（`sched_pick()` 的 `rr` 是每核的，round-robin 本身看着没问题，
   怀疑点更可能在 `sched_switch()` 里 `spin_unlock / plat_wake_cpu / spin_lock` 那段的
   交接时延）。
3. 修上面两个真 bug（`exec` 挂父时机、`laggards` 单槽）。

### 复现用的测试台（`user/smp.c`，未提交）

- `smp -s <n> <ms>`：n 个 worker，全部等完，**一行**汇总。汇总行必须一行，因为多 worker
  同时 `printf` 会被控制台丢行。
- worker 每轮检查 `getcpu()` 是否落在 `{0,1}`；落外面就打
  `worker N: BAD c=%d next=%d x%d (nr is %d), calls=%u` —— `next` 是紧跟的复测值，
  用来区分「机制坏了」和「只丢了这一个值」。
- worker 正常行带 `calls`，可以和内核计数器对账。
- 注意：`smp -s 4` 的 `result:` 行目前**不可信**（`laggards` 单槽，见下），
  要看单 worker 的 `worker N:` 行。

## 23. RP2350 双核 SMP 竞态审计（2026-11-08；check-then-sleep 已修，§22 帧 bug 未修）

§22 追的是一个具体现象（`getcpu()` 偶尔返回系统调用号）。这一节换成审计视角：
把跨核共享状态、原子性、屏障、唤醒时序全部过一遍，给出判据、结论和补丁。
代码基线 `a1d8c69`（HEAD），板子 RP2350 双 Cortex-M33，UART0/COM6。

### 23.1 数据竞争与屏障：先给结论

| 检查项 | 结论 | 判据 |
|---|---|---|
| `spin_lock` / `spin_unlock` 缺 DMB | **不缺** | `arch/arm/platform.c:491/505/511` 三处 `dmb`：acquire 成功 STREX 之后、trylock 之后、release 之前 |
| LDREX/STREX 序列本身 | 正确 | 失败即重试；`trylock` 失败路径补 `clrex`，避免留下独占标记 |
| 释放用普通 store（不是 STREX） | 可接受 | 独占持有期间唯一写者，`dmb` 已保证前序写可见 |
| 缓存一致性 | **不是问题** | RP2350 的 M33 核无数据 cache，SRAM 访问到总线即一致；`XIP` 的指令 cache 只影响自修改代码，内核不做 |
| 编译器重排 | 已处理 | 锁原语带 `"memory"` clobber；共享字段无 `volatile` 是靠锁边界的 DMB 而不是靠 volatile |
| ISB 缺失 | 不适用 | 只有改 CONTROL/MPU/VTOR 之后需要，`platform.c:807/board/rp2350:861` 已有 |

**所以"加打印就消失"不是屏障问题。** 这个 Heisenbug 的形状是「时序窗口被打印的
串口时间放大了」，不是「缺 dmb 导致读到自己写的旧值」——后者是确定性的，不会随机。

### 23.2 真正的竞争：check-then-sleep 不原子（不是"帧写在锁外"）

**先纠正一个容易被当成根因、其实次要的点。** `make_ready()` / `block()` 确实是
"帧在锁外写、状态在锁内写"，但 `plat_spin_unlock` 的 release `dmb` 把这个帧写和
unlock 绑在了一起，所以任何**通过锁**读 `saved` 的观察者都是安全的（帧写 → dmb →
unlock store；对方 acquire store → dmb → 读帧，传递性成立）。

唯一不取锁的读者是 `sched_pick()`（`process.c:339` 直接读 `procs[idx].state`）。
它依赖"同一核的两个 store，别的核按程序序看到"——ARMv8-M 对 Normal memory
**不保证**这一点，但这台板子的总线是顺序的，所以这属于"架构上不保证、实测看不出"
的一类。**它不是 Heisenbug 的来源，不要在这上面耗时间。**

真正能造成无限期卡死的是另一件事：**「查谓词」和「置 state」不在同一个临界区**，
而且 waker 用状态反查自己该不该动手。三处，全是同一形状：

| 位置 | 检查（锁外） | 睡眠 | waker 怎么找它 | 丢了会怎样 |
|---|---|---|---|---|
| `process_wait()` `:1597` | 扫 `procs[]` 找活着的子进程 | `block(ST_WAIT_CHILD)` | `reap_into_parent()` `:1536` 读 `state == ST_WAIT_CHILD` | **永久睡**（无兜底） |
| `process_read()` FD_PIPE_R `:2277` | `pipe_read()` 返回 `EAGAIN` | `block_on_pipe()` | `pipe_wake()` `:2164` 扫 `state == ST_WAIT_PIPE` | **睡到下一次写才醒** |
| `console_read()` `:2214` | `tty_line_ready()` 假 | `block(ST_WAIT_INPUT)` | `process_console_kick()` `:576` 扫 `state == ST_WAIT_INPUT` | 1 ms 后自愈 |

时间线（以 `wait` 为例，`-s 4` 时最容易撞上）：

```
CPU A: process_wait() 扫到子进程 c 还活着，正准备 block()
CPU B: c 退出 → reap_into_parent() 读 parent->state，还是 ST_RUNNING
       → 走 else 分支，code 进环/槽，没人唤醒
CPU A: block() 置 ST_WAIT_CHILD → sched_switch → 永远没人来叫它
```

**这解释了"为什么只有 `wait`/`pipe` 卡死、控制台从来不卡"**：控制台的 kick 在
`process_tick()` `:624` 里每 1 ms 重跑一遍（`cpu_id()==0` 时），丢一次 1 ms 后补上；
`wait` 的 `reap_into_parent` 只在子进程退出那一刻跑一次，`pipe_wake` 只在写那一刻
跑一次，**都没有周期性兜底**。

正确的结构是：谓词检查和 `state` 赋值在**同一个临界区**里，并且 `block()` 必须在
锁内**重查**一次——"结果可能已经躺在环里了，那就别睡，直接返回"。

```c
uint32_t irq = plat_irq_save();
spin_lock(&proc_lock);
if (已有人替我落了结果) {          /* 重查，这一步是关键 */
    spin_unlock(&proc_lock); plat_irq_restore(irq);
    return done(r, 结果);
}
p->saved = *r;                      /* 帧和状态现在同属一个临界区 */
plat_frame_set_ret(&p->saved, (uint32_t)-1);
p->state = ST_WAIT_CHILD;
p->cpu = -1;
spin_unlock(&proc_lock);
plat_irq_restore(irq);
return sched_switch(r);
```

`stash@{0}` 那个补丁**只把帧写挪进了 `block()` 的锁，没有做重查**，所以它只把窗口
从"几十条指令"缩到"几条指令"，**没有关掉**（见 23.6）。按上面这个形状重写的版本
已经落地并通过真板 A/B，见 23.10。

### 23.3 拿锁必须连带关中断，否则是自死锁

这一条不依赖任何吞吐数字，从代码里就能看出来——**给 `block()`/`make_ready()` 加
`proc_lock` 之后，那个临界区必须关中断**：

```
本核持有 proc_lock（中断开着）
        → SysTick 打断
        → process_tick() → 唤醒睡眠任务 → make_ready()
        → spin_lock(&proc_lock)   ← 这把锁是自己持有的
        → 自旋到 SPIN_LIMIT（4 000 万）
        → raw_puts("[spin] cpu N stuck on lock ...") + plat_panic_halt()
```

`block()` 在系统调用上下文里中断是开着的，所以这个窗口一直存在；`plat_spin_acquire`
撞到 `SPIN_LIMIT` 不是"放弃"，是**打印并停机**（`arch/arm/platform.c:477-489`），
所以它是硬故障而不是性能问题。

> 记录上要分清：这条是**代码推出来的必然性**。当时 `-s 2` 量到 1 411 的那一版确实
> 只加了 `spin_lock`，但那一轮没有留下 `[spin]` 行的记录，所以**不能把这个数字
> 归因给自死锁**（见 23.6 的污染问题）。结论不变，证据分级要说清楚。

正确写法是**save/restore 而不是无条件 disable**——因为 `make_ready()` 也会被 tick
调用，那里中断本来就是关的，无条件 `plat_irq_enable()` 会把 tick 处理器的中断提前打开。

```c
uint32_t irq = plat_irq_save();     /* ARM: mrs r,%0,primask; cpsid i */
spin_lock(&proc_lock);
...帧 + 状态...
spin_unlock(&proc_lock);
plat_irq_restore(irq);              /* 原来关着的，走的时候还关着 */
```

`plat_irq_save/restore` 是新加的原语（`kernel/platform.h`，ARM 实现读 PRIMASK，
其余端口 weak 默认退回 disable/enable）。

### 23.4 原子指令、锁正确性、优先级反转

| 项 | 结论 |
|---|---|
| 原子 RMW | ARM 侧只有 LDREX/STREX，没有 `ldadd`。**任何 `x++` 形式的跨核计数都不是原子的**——本轮的诊断计数器（`arm_r_calls` 等）在双核下实测会少记（`writes` 比 `calls` 少 10 是丢增量，不是逻辑错） |
| `cpu_count++` | 非原子，但只有每个核启动时各加一次，且第二次核启动时第一次已完成，实际无害；**将来加核需要改成锁内** |
| 锁的公平性 | 无排队，`plat_spin_acquire` 是抢；持锁时间都在几十条指令内，实测无饿死 |
| 优先级反转 | **存在但不可达**：自旋锁持有期间不阻塞、不调度（临界区内没有 `sched_switch`），所以低优先级任务不会被高优先级任务在锁上挡路 |
| 中断优先级反转 | SysTick 与 SVCall 同为可配置优先级；tick 里要拿 `proc_lock`，所以**必须**用 23.3 的 irqsave 形式，否则就是自死锁而不是反转 |

### 23.5 IPI、tick 同步、WFI 唤醒时序

| 项 | 实际实现 | 风险 |
|---|---|---|
| 核间唤醒 | `plat_wake_cpu()` = `sev`（`arch/arm/platform.c:398`） | **等于没有 IPI**。`sev` 只置位事件寄存器，能唤醒 `wfe`；本端口根本不用 `wfe`/`wfi`，idle 是忙轮询，所以 `sev` 无效果。跨核派发的延迟上限 = 一个 tick（1 ms） |
| tick 同步 | 每核独立 SysTick（`plat_timer_start_cpu()` 写 `CTRL=7`），各自 1 kHz，**不锁相** | 两核的 `plat_ticks()` 不同步；所有超时都是相对量（`now - wake_tick`），所以不影响正确性，但同一任务的 `cpu_ms` 统计在两核间不完全可加 |
| 双核 WFI | **不用 WFI**，`plat_cpu_idle()` 只有 `cpsie i` | 文档写明这是刻意的：WFI 在 MemManage 之后丢唤醒且无法解释。代价是一个核永远在跑 |
| `need_resched` | 每核一份（`cpus[i].need_resched`） | 跨核 `make_ready` 只会置**发起核**的标志，目标核要等自己的 tick。不是 bug，但派发延迟同上 |
| 抢占边界 | `arm_handle_tick()`：`if (!from_user) return f;` | 正确，且是必需的——内核态帧不具备可恢复性（见 23.7） |

### 23.6 修复补丁：改了什么、为什么没合并

补丁在 `git stash@{0}`（`smp-audit-wip`），也导出到 `/tmp/smp-patch.diff`（609 行）。
六个 hunk：

1. **`reap_into_parent()` 加锁 + 用环形槽记 (pid, code)**。原实现是
   `int exit_code; int laggards;` 一个槽，多孩子先退出时互相覆盖 —— §22 记的
   `w1=w2=w3=27` 就是它。改成 `LAGGARD_MAX=4` 的环形队列，溢出记 `lag_drop`
   而不是静默丢弃。
2. **`wait()` 从环里按 pid 取**，取走时移位收口，全程在锁内。
3. **`process_exec()` 把挂父挪进 `proc_create_claim()` 的发布临界区**（新增
   `parent` 参数）。原来先发布 `ST_READY`、再 `p->parent = current`，中间另一核
   可以把子进程跑完并退出，退出码丢失，槽位复用后这一行还会写到别人的 `parent` 上。
4. **`block()` / `block_on_pipe()` / `make_ready()` 的帧写进锁**（23.2）。
5. **`plat_irq_save/restore` 新原语**（23.3）。
6. `proc_create` 的调用点跟着改签名。

**补丁缺一块，所以不能合**：它把帧写挪进了锁，但 `process_wait()` /
`process_read()` / `console_read()` 里那三个"查谓词 → 睡"的序列仍然在锁外串着走，
`block()` 也没有在锁内重查一次。也就是说 §23.2 的表里那三行**一行都没被关掉**，
只是窗口变窄。补丁 3（`exec` 挂父）和补丁 1/2（环形槽）本身是对的、无争议，
可以单独立项先合。

**另外它把诊断探针一起打进去了**（`arm_resume` 里的四个计数器），所以下面的
"补丁后"一列量到的是"修复 + 探针"，不是修复本身。

**效果（真板实测）：**

| 指标 | HEAD | 补丁后 |
|---|---|---|
| `smp -s 2` 退出码与块数一致 | ✅ | ✅ |
| `smp -s 4` `w0` = 栈地址（537116144） | **复现** | 不再出现（本轮样本） |
| `smp -s 2` 吞吐 | 5 957 ~ 6 522 | **1 411（4 倍退化）** |
| `smp -s 4` 吞吐 | 53 / 3 698 / 5 563 | 90 ~ 1 523 |

**但这个 1 411 是脏数据，不能当作"修复的代价"。** 参与 A/B 的"补丁"里同时包含了
§22 的诊断探针——`arm_r_calls` / `arm_r_writes` / `arm_r_hret` / `arm_svc_nr25`
四个计数器的自增**全部加在 `arm_resume` 里**，而 `arm_resume` 是整台机器上最热的
代码（每一次异常返回都过，即每个 syscall + 每个 tick）：

```asm
    ldr     r2, =arm_r_calls        @ 4 个计数器 ×（ldr + ldr + adds + str）
    ldr     r3, [r2]
    adds    r3, #1
    str     r3, [r2]
    ...
    ldr     r2, [r0, #FR_VEC]       @ 外加两条判据分支
```

三个计数器还是**两个核同时 RMW 的同一批字**。所以 4 倍退化里有多少是锁、有多少是
探针，**这一轮根本没分开**。

结论要改成：**这个数字不作数。** 要判定修复的真实代价，必须把探针剥掉重测（探针
只留一个"输出后自锁"的版本，见 23.7 的判据纪律）。在剥离前，说"修复慢 4 倍"是
不成立的；同样，说"修复不慢"也不成立。

### 23.7 最小复现方案

内核不用改，只要打两个补丁点（都在 `user/smp.c`，已提交）：

1. **判定"写回是否发生"的回声**。寄存器残留会伪装成"写回成功"，所以不能只看
   标记值。让内核在 `SYS_GETCPU` 里把调用方自己的 `r2` 回写到 `r12`
   （`0xAB000000 | seq`），用户侧一次 `svc` 读回 `r0/r3/r12/r2` 四个寄存器：

   ```c
   register int r0 __asm__("r0") = SYS_GETCPU;
   register uint32_t *p __asm__("r1") = o;
   register uint32_t r2 __asm__("r2") = seq;
   __asm__ volatile("svc #0\n\t"
                    "str r0, [r1, #0]\n\t"
                    "str r3, [r1, #4]\n\t"
                    "str r12, [r1, #8]\n\t"
                    "str r2, [r1, #12]"
                    : "+r"(r0), "+r"(r2) : "r"(p) : "r3", "r12", "memory");
   ```

   判据：`r12` 的序号等于**本次** `seq` → 写回发生了；等于 `seq-1` → **整帧都没写回**，
   用户拿到的是 `svc` 入口寄存器（这正是实测结果）。

2. **`smp -s <n> <ms>`**：n 个 worker 各自跑 `<ms>` 毫秒，父进程等全部结束后
   **一行**汇总。汇总必须一行，因为多 worker 同时 `printf` 会被控制台丢行（#78）。

```
smp -s 1 500     → 干净（4 次）
smp -s 2 500     → 干净（4 次，约 50 000 次调用）
smp -s 4 500     → 每次都能复现至少一条 BAD 或一个栈地址退出码
```

**触发条件是"任务数 > 核数"**，这是最有价值的线索：它说明缺的是一次
"有任务在等、但没被派发到另一个核"的窗口，而不是单任务路径的 bug。

**两种故障要分开记，别混成一个**：

| 现象 | 属于 | 判据 |
|---|---|---|
| `worker N: BAD r0=...` / 退出码是栈地址 | §22，帧被恢复的地址不对 | `-s 4` 每次都能出（HEAD 基线） |
| `smp -s 4` 跑完不回提示符、或 `smp -s result:` 永远不出现 | **§23.2 丢唤醒** | 父进程停在 `ST_WAIT_CHILD`；`ps` 能看到 `wait` 状态的任务挂着 |

### 23.8 已排除清单（都带判据，别再重复挖）

| 假设 | 判据 | 结果 |
|---|---|---|
| `arm_resume` 走了"没有硬件帧就跳过写回"的分支 | 计数器 `arm_r_skips` | 0 |
| SVC 被 pending 启动分支劫持 | `arm_pend_hits` | 整轮只 1→2，与异常无关 |
| 内核把 `r0` 还是 25 的 svc 帧交出去 | `arm_svc_nr25` | 0 |
| tick 叠进 SVC 处理器造成丢写回 | 在 `SYS_GETCPU` 里 `plat_irq_disable()` 后再写 | **无效**，BAD 照出 |
| `plat_cpu_id()` 返回 25 | `SIO_CPUID` 是每核常量；让 case 把 25 改成 99，用户从没见到 99 | 否 |
| 用户侧代码生成把 `r0` 读早了 | `clang -Oz -S` 实测 `movs r0,#25 / svc #0 / str r0,[r1]` 紧挨 | 否 |
| 同一个任务被两个核同时选中 | `sched_pick()` 返回 `cpu != -1` 的任务：`probe.pick_busy` | 9 万次选取 0 次 |
| 锁缺屏障 | 见 23.1 | 不缺 |
| `sev` 唤不醒核 | idle 不是 WFI，`sev` 本就多余 | 不适用 |

### 23.9 结论

- **确认的缺陷（按优先级）**：
  1. **check-then-sleep 不原子**，三处（`wait` / pipe 读 / 控制台读），前两处
     没有周期性兜底 → 永久睡。这是"随机卡死"最可能的根因，也是"任务数 > 核数
     才触发"的最佳解释。
  2. `reap_into_parent()` 完全无锁 + 单槽 `exit_code/laggards`（多孩子退出码互相
     覆盖，§22 的 `w1=w2=w3=27`）。
  3. `process_exec()` 先发布 `ST_READY` 再写 `p->parent`。
  4. `block_on_pipe()` 的 `state` 写在任何锁之外。
  5. `sev` 是假 IPI（idle 不是 WFI），跨核派发延迟上限 = 1 tick。
- **确认不是缺陷**：锁屏障（`dmb` 齐全）、缓存一致性（M33 无数据 cache）、
  `plat_cpu_id()`、pending 路径、双跑（9 万次选取 0 次）、tick 锁相。
- **判据上不成立、不要当根因**：`sched_pick()` 裸读 `state` 与帧写之间的
  store-store 次序——架构上不保证，总线上是顺序的，实测不出。
- **不作数的数据**："修复慢 4 倍"。参与 A/B 的补丁里带着 `arm_resume` 的四个
  计数器，热路径被污染，**必须剥离探针重测**——剥离后实测没有退化，见 23.10。
- **仍未定性**：`getcpu()` 偶发拿回入口帧（`r0`=系统调用号、`r12`=上一次回声）。
  所有"内核写错值"的可能都被计数器排除了，剩下的唯一方向是**帧被恢复的地址
  与写入的地址不一致**——下一步不是再猜，而是把 `hw`（硬件帧地址）本身用回声
  打出来，比对"写回的帧"和"恢复的帧"是不是同一个。

### 23.10 修复落地与真板 A/B（2026-11-08）

按 23.2 的形状重写，**不带任何探针**，只动内核：

| 改动 | 位置 |
|---|---|
| `plat_irq_save/restore` | `kernel/platform.h` + `arch/{arm,rv32,x86}/platform.c` |
| `block_locked()`：帧 + 状态在调用方已持有的锁里 | `kernel/process.c` |
| `process_wait()`：扫子进程与入睡同一个临界区 | 同上 |
| `retire_and_reap()`：reap 与 retire 一步（父进程再也看不到"既没在等、槽也没空"） | 同上 |

`plat_irq_save` 三端口各自实现，没有走 weak 默认：默认的
`disable(); return 0;` 在 tick 上下文里会把"本来关着的中断"在 restore 时打开，
正是 23.3 要避免的那件事。

**A/B 条件**：`-target rp2350smp`，只差内核（同一个 `user/smp.c` 测试台）；
`scripts/rp2350/smptest.py` 每轮 11 条命令。

| 项目 | HEAD 基线 | 修复后 |
|---|---|---|
| `smp -s 2 500` 块数（3 轮） | 6095/6469 · 6077/6444 · 6096/6464 | 6501/6985 · 6978/7011 · 6500/6937 |
| `-s 4` 出 `BAD r0=00000019` | 3/5 | 4/10 |
| `-s 4` 退出码是栈地址（537181680） | 出现 | 出现（2/10） |
| `-s 4` 挂死（无 prompt） | 0/5 | 0/10 |
| 功能 7 项（echo/ls/free/ps/管道/py/sleep） | — | 7/7 |

三条结论要分开说，别合成一句：

1. **§22 的帧寻址 bug 一点没动**：`r0=00000019`（=25，`SYS_GETCPU`）、
   `r12=00000035`（上一轮回声的序号）与基线完全一致。它才是 `-s 4` 上唯一
   可见的故障，也是"退出码变成栈地址"的来源。
2. **没有 4 倍退化**：`-s 2` 从 6 077~6 469 到 6 500~7 011，同量级（修复后这一侧
   甚至略高，但只有 3 轮样本，不当结论）。上一轮的 1 411 全是探针的账——四个
   计数器都加在 `arm_resume` 这条每次异常返回都走的路径上，而且两核同时 RMW
   同一批字。
3. **"随机卡死"在这块板子上没有复现**：21 次 `-s 4` 一次没挂。丢唤醒的时序是
   真的、修复按构造是正确的，但它不是这台机器上可见故障的来源。同一形状**还没修**
   的两处是主要嫌疑：管道读（`pipe_wake`）和 console 读（`process_console_kick`）。
   前者一次写入才 wake 一次，丢了就是永久睡；后者靠 `process_tick` 每 1 ms 重踢
   兜底，所以从没表现出来。

其余回归：x86 / rv32 / rv32smp 的 QEMU 冒烟全过（rv32smp 报 `cpus=2`、`prot`/
`crash` 的 fault 隔离都在）；C3 真机 `accept.py` **24/24**。

顺带修掉一个坑：`scripts/esp-c3/flash.py --build` 调的是仓库根目录里那个
gitignore 掉的 `tos.exe`（2026-11-02 的陈货），它会用一个旧构建工具链编出
"kernel overflows PLAT_KERNEL_MAX" 的假失败，而同一棵树用 `go run ./cmd/tos`
编得过。已改成 `go run`。


## 24. 管道（2026-11-08）

§23 只修了 `wait()` 一处 check-then-sleep，形状相同的 pipe 读、以及 pipe 环形缓冲
本身都没有锁。这一节是把它修完，同时记下这轮**没有**修好的东西。

### 24.1 改了什么

| 改动 | 位置 | 理由 |
|---|---|---|
| 环形缓冲与"我睡了"同一个临界区 | `process_read`/`process_write` 的 `FD_PIPE_*`、`block_on_pipe_locked` | 写者写完才 wake，读者检查完才睡，两者必须在同一把锁里，否则窗口内丢唤醒 |
| 一次事件唤醒**所有**能推进的等待者 | `pipe_wake_locked`/`pipe_deliver_locked` | 原来只送一个：两个读者停在同一管道上，写端一关，第二个永远等不到 EOF |
| 引用计数自带锁 | `pipe_ref_*`/`pipe_drop_*`，`struct pipe` 加 `spin_t` | `fd_clone()` 在 exec 路径上取引用，**不持** `proc_lock`；裸 `++` 与另一核的 `--` 丢更新 → 写端还活着读者就看到 EOF、写者拿到 EPIPE |
| 关端与 `kfree` 分开 | `fd_close_in` | 计数归零与广播在锁内，`kfree` 在锁外（此时已无任何 fd 指向它） |
| `wait()` 记 (pid, code) | `proc_t.lag_pid/lag_code`，`LAG_MAX 8` | 单槽 `exit_code`+`laggards` 在多个孩子先退出时互相覆盖 |

引用计数那把锁的第一版只在 exec 侧上锁、**没有关中断**，真板上表现为两核同时
卡死：`fd_clone` 持 `p->lock`（中断开着）→ tick → `make_ready` → 要 `proc_lock`，
而持 `proc_lock` 的那一核正在等 `p->lock`。同一个 ABBA 形状，§23.3 的规矩照抄：
**拿锁必须连带关中断**。加上之后 `piperace 4 32` 从 1/6 变成 5/6。

### 24.2 `wait()` 为什么需要一张表

`piperace 2 64` 一次起 4 个孩子，全在父进程第一次 `wait()` 之前退出。旧的
`parent->exit_code = code; parent->laggards++;` 只留最后一个，父进程随后每个
`wait(pid)` 拿回的都是同一个（错）值，或者干脆 -1。真板上 `w=-1 r=-1` 是这个
原因，不是管道。

换成 8 个 `(pid, code)` 槽之后同样的命令 4/4 正确。代价是 `proc_t` 长了 64 B
（换掉 `exit_code`+`laggards` 的 8 B），C3 上 `procs[8]` 多占 512 B，仍有 17 KiB
余量（§20）。

### 24.3 用户态：短写会丢数据（`cat | wc` 的字节数一直是对的错觉）

`write()` 在管道上满环就返回短计数，而 `cat`/`puts`/`printf`/`putint` 都不看返回
值，直接把剩下的丢掉。所以 `cat demo.py | wc`（2788 B 的文件）在 HEAD 上三次跑出
**2788 / 2660 / 2532** 三个数——不是内核丢的，是 `cat` 丢的。

加 `write_all()`（`user/lib.c`）让 `puts`/`printf`/`putint`/`cat`/`sh` 都走它之后，
同一命令固定 2788，`cat pytest.py | wc` 固定 3447，正好是文件大小。

### 24.4 ~~仍未结案~~：>50 KiB 之后字节错位（HEAD 也有）

> **已结案（2026-11-08）**：第一因是“tick 抢在 SVC 前面、SVC 被 pended 时内核仍
> 切换任务”。修复与验证见 §24.4.5 末尾。

`piperace 2 64`（两对，各 64 KiB）在本树上 **5 次里 4 次失败**，HEAD 上同样失败
（1 次损坏 + 3 次直接卡死）。证据：

```
piperace: bad byte at 61824: got 11 want 139
```

`pat(off) = off*37+11 (mod 256)`，而 37*2048 = 75776 是 256 的整数倍，所以**图案
以 2048 字节（正好是环大小）为周期**。`pat(0)=11`、`pat(61824)=139`，收到的字节
属于 `0 mod 2048` 那一类，而位置 61824 属于 `384 mod 2048` —— 即流**错位了 384
字节（3 个 128 字节块）**。错位发生在 40~60 KiB 之后，1 对 64 KiB 不出现。

#### 24.4.1 第一个根因已修：M33 双核 LDREX/STREX 不互斥（2026-11-08）

rp2350smp 上 `proc_lock` 的自旋实现是 LDREX/STREX。Cortex-M33 **没有全局排他
监视器**——排他监视器是每核一份的，核 0 的 LDREX 在核 1 眼里不存在，STREX 永远
成功。两个核于是能**同时持锁**：`[L race]` 探针（持锁者互相发现对方也持锁）从
持续增长到修后归零，环记账 `[A hit]` 同步归零。这是本条错位形态的主根因。

修法：SIO 硬件 spinlock（`arch/arm/platform.c`，`TOS_ARM_SIO_SPIN` 由 build.go
对 rp2350 板定义）。不能全程持有（内核多锁嵌套会自锁），所以 SIO 锁（#31）只作
3 条指令的短门闩 + PRIMASK 屏蔽，保护 flag 的 test-and-set：外层 `while (*v != 0)`
带 SPIN_LIMIT 卡死报告，门内 claim → dmb → `if (*v == 0) { *v = 1; }`。
SIO 寄存器属 SoC 外设区（0xD0000000），不走 L1 cache，无需维护一致性。

顺带修的：`cpu_secondary_main` 的 CPU 归属判据、`process_resched` 的用户帧门
（tick 落在内核 Thread+MSP 帧上不再消费 `need_resched`，§22 链路的守护）。

**踩过的坑（EXC_RETURN 编码）**：v7-M 的 bit3 = mode（1=Thread）、bit2 = SPSEL
（1=PSP）。`0xFFFFFFF1`=Handler+MSP、`0xFFFFFFF9`=**Thread+MSP**（内核启动路径
kmain/cpu_take_task/arm_enter_user）、`0xFFFFFFFD`=Thread+PSP（用户任务）。
一度把 frame_is_user 改成测 bit3，结果 Thread+MSP 的启动帧被判成 user，tick 在
shell 启动路径上直接 sched_switch，从启动帧恢复任务 → HardFault，QEMU mps2 三个
镜像全挂。QEMU 三步二分定位后回滚：**测 bit2 本来就对**（regs.h 注释已修正）。

#### 24.4.2 SIO 修复后仍有残留（根因未定位）

> **2026-11-08 更新**：其中“-1 预置丢结果 / EPIPE 风暴 / r0 仍是 syscall 号”的
> 主形状已由 §24.4.5 的 pended-SVC 修复解决（单核 RP2350 `piperace 4 64` 10/10）。
> 本条剩下的偶发坏字节/短流仍在 `rp2350smp` 上以约 1/7 的频率出现，是另一条
> SMP 残留，尚未定位。
>
> **已抓到的 SMP 残留现场**（`piperace 4 128`，修复后的双核镜像）：
>
> - 某对写端退出码 **139**（`arm_handle_fault` 的 fault 退出码），读端先报
>   `bad byte at 22144: got 11 want 139`——`got` 正好是 `off-128` 处的字节，
>   即**一个 128 字节块被重复写入**；随后读端 EOF，写端 EPIPE。
> - 但 r0/r12 echo 探针（`plat_frame_set_ret` 写 r12）在这些失败里**没有报**
>   mismatch。说明用户拿到的返回值与 echo 一致，问题不是“结果没写进帧”，而更像
>   **同一次 write syscall 被回放**（pc 回到 svc 之前）或帧的 pc 被另一条路径
>   覆盖。
> - 事件探针显示健康 pair 的 EOF/最后 close 会覆盖失败 pair 的场景；需要按
>   pipe 指针/pid 做环形记录才能对上。
> - 探针加上后 15 轮不触发（时序敏感），源码补丁留在 `build/smp-probes.patch`。
>
> **2026-11-08 深夜：SMP 残留已定位并修复。**
>
> 用“用户 write/read 调用数 vs 内核 dispatch 数”的计数对账（只在 dispatch
> 入口 +1，开销可忽略）抓到了实锤：一次失败里读端 **753 次 read 调用、
> 内核只 dispatch 了 752 次**——**有一次 svc 没进 dispatch**，与之前
> “128 字节块重复 / 写端 fault 139”是同一根源。
>
> 根因是 §24.4.5 的窄判据漏洞：pended SVCall 会被**更高优先级的 pending
> 异常从 VECTPENDING 里盖掉**（常见的是紧随其后的第二个 SysTick）。此时
> `VECTPENDING == SVC` 不成立、`PENDSVSET` 在 M33/QEMU 上又不可靠，tick 仍然
> 切走任务，SVC 留在 pending；tail-chain 后在别的任务帧上执行，少掉一次
> syscall 结果。
>
> 修法：`arm_handle_tick` 在 `from_user` 之后，只要
> `ICSR.PENDSVSET` 或 `VECTPENDING` 的任意一位非零，就**不 reschedule**，
> 本 tick 直接返回。代价只是把抢占推迟到下一个 tick（通常下一拍就恢复）。
>
> 验证（同一镜像、真实板）：
>
> | 场景 | 结果 |
> |---|---|
> | `rp2350smp` + `run spin &` + `piperace 4 128`（计数探针） | 50/50 |
> | `rp2350smp` + `run spin &` + `piperace 4 128`（干净镜像） | 20/20 |
> | `rp2350smp` 无负载 `4 64` + `2 64` | 15/15 |
> | `rp2350` 单核 `4 64` | 5/5 |
> | QEMU mps2 单核 `4 64` | 5/5 |
>
> 修复前同一负载下窄判据版本在 1～10 轮内就会失败。探针补丁仍留档在
> `build/smp-probes.patch` / `build/smp-probes2.patch`。

`piperace 4 64` / `2 64` 偶发 FAIL，单 pair `1 64` 3 连过 → 并发相关。形态：

- 写端 mid-stream 返回 -1（failcode kind4, a=1；PIPE_EAGAIN=-1 与 EPIPE 经
  `pipe_deliver_locked` 的映射不可区分，但 `[E ep]` 持有者对账探针命中过
  `rhold 0`——reader 的 fd 确实没了 = reader 自己先退，writer 引用没丢
  （`[E wdrop]` 修正假阳性后零命中））。
- reader 提前 EOF（kind5：读到 26/35/41/49 KiB 就 0）。
- 流移位：off≈31488 处期望 `pat(off)=11` 得 107（坏字节，非错位 2048 周期）。
- p3 一次 off=0 双失败（`w=80001004`/`r=80000405`）：两个孩子开局即 EBADF/EOF
  形状，指向"child fd 全关"或"任务被重启"一类机制。
- `w r0=1 echo=00000000`：任务从入口帧直接恢复（`plat_frame_set_ret` 未应用）。
- 一次退出码 `0x80`：不属于 137/139/exit(a0) 任一已知来源。

已排除：writer/reader 引用计数丢失、fd→pipe 对象错配、argv 解析（0x7E 探针未
触发）、SIO 锁竞态本身（`[L race]`/`[A hit]` 归零）。

候选方向：`sched_switch` resume 路径的帧来源、`block_on_pipe_locked` 预置 -1
被错误唤醒消费、启动槽协议。撤探针前应先给预置返回值加标记位区分"预置未覆盖"
与"deliver 覆盖"。

#### 24.4.3 QEMU mps2 复现与排除清单收窄（2026-11-08 晚）

QEMU mps2（单核）上 `piperace 4 64` 稳定复现 0/4 → 加 marker 后 2/4，QEMU 也
丢行（推翻"QEMU 不丢行"）。本轮新证据与排除：

- **探针自污染定案**：arm_resume 尾部 C 探针的局部变量必然覆盖活帧低 32 B，
  幽灵帧证据全部作废；改纯 asm ring 捕获（label 7，64×16 字）。
- **窗口期彻底排除**：ring 检查 `*hw+0 == frame.r0` 且 `*hw+16 == frame.r12`
  （r12 槽盲区已补），全部自洽——cpsie i → bx lr 之间无人写坏帧。
- **-1 预置唤醒分类**（`w1=` 按 src 计数）：命中全部 src=4（合法 EPIPE 投递
  给 writer），src=2/3/5（丢失唤醒形状）= 0 —— 丢失唤醒假说被否。
- **`[B big]` 是误报**：er 环的巨大 r0（0x8c819405 等）= 任务的 exit code
  （failcode/offcode），set_ret 合法写入，echo 自洽。
- **`[N hw2]` 窗口观察者是误报**：任意嵌套 tick 读 PSP = 用户栈顶活动数据
  （r12=KEY^1 是 shell echo 变量残留），已撤。
- **`[P prev]` 0 命中**：sched_switch 抢占复制无 procs[0] 交叉写入。
- **argc/argv 损坏实锤（偶发）**：子进程报告 `0x7E000001`（argc<4 而
  argv[1] 完好）+ `w=00000001`（is_child 误判为 false → 走 parent 路径
  return bad=1）。piperace 加 "C" 标记参数 + argc/argv[1]/argv[2] 校验后
  通过率 0/4 → 2/4。**`[E argc]` 写后读回验证未命中** → 损坏发生在布局
  之后、crt0 之前（或 crt0 的 sp 错位）。
- **exit code 0 形状**：`w=00000000`/`r=00000000` 与"子进程走 parent 路径
  return bad=0"吻合；`w=8xx01004`（EPIPE@0-9 KB）与 reader 提前死级联。
- **proc_retire 泄漏 fd 引用已修**（exec 晚期失败路径，永久候选）。
- **HardFault 转储**（fault 任务 r12=0xa5a5a5a5 = -1 预置 echo、r9=0x1999999a
  = printf ÷10 魔数、故障在 `strh [r0,#12]`）：echo 失配 printf 路径崩溃，
  fault 任务被 exit(139) 处理；转储已加 `[F pid=...]` 尾行（console 丢行
  只吃行首，关键信息放最后）。

**当前第一嫌疑**：exec 初始栈（argc/argv/strings，build_initial_stack →
plat_prepare_user_frame → arm_enter_user 启动链）在窗口复用/连续 exec 下的
偶发损坏或 sp 错位；其次是假 EPIPE（readers 计数错误清零，`[E ep]` 场景已
入 dbg 行 `ep:` 字段，待命中）。

#### 24.4.4 二分推进（2026-11-08 深夜）

- **假 EPIPE 被否**：`epd=` 场景（pid/gen/r/w/cnt/rh/rpid/rfd 并入 dbg 行）
  显示 readers=0 且持有者扫描 rh=0 —— **计数与持有者一致，reader 真的先退
  了**，writer 的 EPIPE 是对的。第一因回到"reader 为什么提前死"。
- **argc 槽损坏确认 + 部分二分**：piperace 子进程报 `0x7E0000A3`（新魔数，
  低 4 位 = argc=3；布局写的是 5）。**[E argc] 写后读回（prepare_user_frame
  之后）从未命中** → 损坏在布局之后；**start-svc 消费点读回探针**（asm，
  [hw+32]）曾见 0 —— 但 `sac=0/bad=1` 恒定来自 **shell 的启动**
  （shell 以 argc=0 布局属正常，探针假设 5 系误报源，已修为只记 !=5 并存
  hw）。piperace 子的"消费点 argc 值"被 shell 的记录覆盖，**需要环形记录**
  才能对上"哪次 start 读到几"。
- **wait 卡死型**：一轮出现每轮 hang——writer（pid 3）已退出（fd 表无它）
  而父进程 fd4 未关（close(fds[1]) 未执行到），reader 停在空管道；父进程
  疑卡在 wait(wpid[0]) —— **writer 的 exit code 未送达（lag 路径待查）**。
- **hw/PSP 一致性检查**（ring 新增条件 psp!=hw）零命中 —— 写回目标无误。
- **探针污染教训再+1**：sac 探针假设 argc=5 把 shell 的正常启动记成坏。
- **临时工具**：scripts/tmp-arm.py 每轮后发 `ps` 抓 dbg:/er: 行；dbg 行含
  pair/race/ufb/wdrop/eof/ep/br/fr/exe/wake/big/w1/epd/sac/tak。

**下一步**：① start-svc 消费点改环形记录（值+hw+pc，8 项）以对上"哪次
start 读到几"；② lag_push/lag_take 的 exit-code 送达路径加对账；③ 查
`p->parent` 在 writer exit 时的正确性。

#### 24.4.5 决定性 A/B：触发条件是 tick 抢占，不是管道（2026-11-08 更晚）

**A/B 结果**：`QUANTUM` 从 1 改成 1000000（tick 不再主动抢占，只保留阻塞时的
自愿切换）后，`piperace 4 64` **2/2 全 ok，echo mismatch 归零**；改回 1 立刻
0/2~0/4。第一因锁定在**运行中的任务被 tick 抢占、之后再恢复**这条路径，与管道
环形缓冲、引用计数、阻塞唤醒、lag 送达都无关。

新增的 per-task 探针（都在 `proc_t` 里，随 `ps` 的 dbg 行输出）：

- `dbg_l*`：本任务最后一次 **syscall 向量（vec=11）** 的恢复帧；
- `dbg_a*`：本任务最后一次**任意**恢复帧（含 tick，vec=15）；
- `dbg_nr_disp/res`、`dbg_rd_disp`：dispatch / syscall 恢复 / read dispatch 计数；
- `svc in/disp/start`：`arm_svc` 进入、进入 dispatch、被 pending-start 吃掉的次数；
- tick-save 捕获：tick 抢占保存的帧若 r0 仍是 syscall 号而本任务上次 syscall 结果是
  另一个值，记下两边（`ts:` 行）。

**现场（每次失败形状一致）**：

```
ts: pid=8 tick pc=20100864 r0=00000002 r12=0000007f
    last r0=00000080 r12=5a5a5ada pc=20100864 v11
```

- 本任务最后一次 syscall 恢复：`pc=win+0x864`（`read_echo` 里 `svc` 之后的第一条
  指令），`r0=0x80`、`r12=0x80^KEY` —— 内核认为结果已经交付。
- 紧接着 tick 抢占保存的帧：**pc 相同**，但 `r0=2`（`SYS_READ`）、`r12=0x7f`
  —— 用户寄存器还是 **svc 之前**的值。即 eret 装上了 pc，却没装上 r0/r12。
- `arm_resume` 尾部新加的持久检查（frame 的 r0/r12、`*hw`、`*hw16`、`PSP` 四者
  全等）**0 命中**：eret 前一刻的写回目标、硬件帧内容和 PSP 都自洽。
- `arm_svc` 计数 `in = disp + start`，所有 svc 都进了 dispatch；但本任务的 read
  dispatch 计数**恒比用户 read 调用数少 1**，与"pc 已过 svc、r0 还是 syscall 号"
  完全一致：有一次 read 的结果从未被用户看到。

**已排除**（本轮）：窗口复用（win_alloc 与 hang dump 自洽）、跨窗口栈
（`fr=0/ufb=0`）、引用计数/假 EPIPE/提前 EOF（无 tick 抢占时全通过）、
`-1` 预置唤醒（`w1` 全 src=4）、写回丢失（上面的四者全等检查 0 命中）。
另外把 echo 指针从 `lr` 换到 `r4`（`lr` 是内核唯一需要从栈帧恢复的用户寄存器），
并在 asm 内直接存 raw r0/r12，排除了"测量被 epilogue 影响"这一层。

**当前唯一嫌疑**：tick 抢占的保存/恢复与 SVC 返回之间的交接。已在 `arm_resume`
尾部把实际写入的 `(pc,r0,r12,psp,hw,*hw,*hw16,exc)` 存成 `dbg_wr_f`，与下一次
tick save 对账：

```
ts: tick pc=200f0888 r0=00000001 r12=00000025
    wr   r0=00000080 r12=5a5a5ada pc=200f0888 psp=hw=200ffd68 h0=80 h12=...
    dwr=1  thw=200ffd68
```

- `dwr=1`：tick 是这次 syscall `arm_resume` 之后**第一个**异常；
- `thw == wr hw`：tick 的 PSP 就是刚写回的帧地址，没有 PSP 漂移；
- 但 tick 保存下来的 `r0/r12` 是 **svc 之前的 1/0x25**，而 `[hw]` 在 eret 前
  一刻确实是 `0x80/0x80^KEY`。

即：**写回目标、PSP、硬件帧内容三者都对，eret 却像只装了 pc/sp、没装 r0/r12**。
这与“tick 落在 `arm_resume` 内部”的形状一致。已试过在 `arm_resume` 入口
`cpsid i`、只在 `bx lr` 前按 exc bit2 重新 `cpsie i`：**启动即 HardFault
(exc=0xfffffff9, pc=0xee38)**，说明不能简单地对所有返回路径无条件改 PRIMASK，
该实验已回退。

**更晚的两组实验（2026-11-08 末）**：

- **marker 实验**：在 `arm_resume` 最后把 `0xDEADBEEF` 写进 user 帧的 r1 槽，
  下一次 tick 的 raw 压栈帧里看得到就说明 eret 真的 unstack 了。结果：
  - 正常恢复：`f1=0xDEADBEEF`（unstack 正常）；
  - 失败的那次：`f1=00000003`（**svc 前的 fd 参数**），marker 不在——
    **eret 没有 unstack 这个写好的帧**。
- **raw SysTick 入口**（在 `arm_trap_enter` 之前记录）：失败 tick 的
  `lr=0xFFFFFFFD, msp=0x20076c10, psp=hw, primask=0`，并且 `psp[0]=2`、
  `psp[4]=3`、`psp[24]=post-svc pc`。即它确实按 Thread+PSP 压栈，且压栈内容就是
  svc 前的寄存器；不是“压栈在 MSP、只是 LR 看起来像 FD”的普通 tail-chain。
- **原子化 A/B**：入口 `mrs primask` 保存、`cpsid i` 包住写回、末尾对 kernel 帧
  `msr primask` 恢复、user 帧 `cpsie i`——启动不再 HardFault，但**仍然 0/3**。
- **PENDSTSET 清除 A/B**：在 user 返回前若 ICSR.PENDSTSET 置位就 PENDSTCLR，
  **仍然 0/3**。所以不是“eret 处正好有一个 pending SysTick”。

**根因（QEMU `-d int` trace 确认，2026-11-08）**：

```
Taking exception 2 [SVC] on CPU 0
...taking pending nonsecure exception 15      <- SysTick 先被取走
...loaded new PC 0xefc1
Exception return: magic PC fffffffd previous exception 15
...tailchaining to pending exception 11        <- 被 pended 的 SVC 之后才执行
```

SysTick 优先级 0x00 高于 SVCall 0x40，所以 `svc` 执行的同一个指令边界上如果有
pending tick，**tick 先进入，SVC 被 pended**（ICSR 现场：`0x0000B80F`
= VECTACTIVE 15 + VECTPENDING 11；注意 QEMU/mps2 **不置 PENDSVSET**）。
这个 tick 的 EXC_RETURN 是 0xFFFFFFFD（Thread+PSP），内核按普通用户 tick 处理，
`process_resched()` 切走任务，并把 **svc 之前**的用户寄存器存进 `saved`
（r0 = syscall 号）。随后被 pended 的 SVC 才 tail-chain 执行，等原任务以后从
这份 saved 恢复时，pc 已经在 svc 之后、r0 仍是 syscall 号——一次 syscall 的结果
就这样丢了。短写/重复块/移位流/EPIPE/argc 与 fd 级联全部由此而来。

**修复**（`arch/arm/platform.c: arm_handle_tick`）：tick 处理完以后，如果
`ICSR.PENDSVSET` 置位，或 `ICSR.VECTPENDING == ARM_VEC_SVCALL`，**不要
reschedule**，直接返回本 tick 的帧。tick 返回后 pended SVC 立即 tail-chain，
在原来的帧上正常 dispatch。判定两个位都查，是因为真机（M33）会置
PENDSVSET，而 QEMU/mps2 只体现在 VECTPENDING。

**验证（QEMU mps2 单核，探针全部撤除后的干净镜像，原 `user/piperace.c`）**：

| 命令 | 结果 |
|---|---|
| `piperace 4 64` | 10/10 + 10/10 + 3/3 |
| `piperace 3 64` | 5/5 + 6/6 |
| `piperace 2 64` | 6/6 + 6/6 |
| `piperace 1 64` | 5/5 |

全部 `piperace: ok`。之前带探针的版本还跑过 `4 32` 5/5。修复前同一镜像每轮都有
错位或 EPIPE。

同时 `x86`、`rv32` 目标都恢复可编译（探针撤掉后 x86 内核重新回到 boot loader
的 92 160 字节限制以内）。`user/piperace.c` 保持原样——旧版用 `dup2` 的测试在
修复后的内核上同样通过（10/10、6/6、5/5）。

**真板 RP2350（ATK-DNRP2350AM，烧录修复后的镜像）**：

| 镜像 | 命令 | 结果 |
|---|---|---|
| `rp2350`（单核） | `piperace 4 64` | **10/10** |
| `rp2350smp`（双核） | `piperace 2 64` | 3/3 |
| `rp2350smp`（双核） | `piperace 4 64` | 19/22 |

单核 RP2350 全过；SMP 上修复前那种“每轮 EPIPE + r0=syscall 号”的风暴消失，
只剩 §24.4.2 记录过的偶发坏字节/短流（约 1/7 次 4×64，3×64 也会偶发），
属于另一条 SMP 残留，不在本次修复范围内。

探针已撤除，工作树已提交（`5de3b30`、`3a1be12`）。

### 24.5 块数吞吐指标在真板上不可用

本轮一度以为引入 5 倍退化（`smp -s 2 500` 从 6700 掉到 1000），复核后**不成立**：

| 镜像 | `smp -s 2 500` | `smp -w 0 500` | `smp -w 1 500` |
|---|---|---|---|
| HEAD | 6753 / 7121 / 6723 | 2953 | 9764 |
| HEAD 内核 + 只换新 `pipe.c` | 4891 / 5389 | — | — |
| 本树 | 926 ~ 1621（8 次） | 2529 | 591 |

HEAD 自己同一条命令的两个 worker 就差 3.3 倍（2953 vs 9764），说明这个数反映的是
**迁移/抢占次数**，不是每次迭代的成本。ramdisk 嵌在内核镜像里，改任何一个用户程序
都会移动 flash 布局，于是同一份内核源码换个用户程序就从 6900 变 5100。

所以：**不能拿它判回归，也不能拿它声称没有退化。** 要量真实吞吐得用 §20 那套
（`bench` + 固定的用户镜像），或者干脆数 tick 里的切换次数。要量**双核加速比**
用 §24.9 的 `smpbench`（固定工作量、测时间，不是块计数）。

### 24.6 资源悬崖：10 个任务时 exec 会失败

`piperace 4 32` = 8 个子进程 + 父 + shell = 10 个任务，偶尔 `exec` 失败（窗口分配
不到）。表现是 `w=0`（写端第一次写就 EPIPE）。子进程现在会在开工前 `close(3..11)`
把自己继承来的另一头关掉，所以伙伴没起来时这边得到 EPIPE 而不是永久挂住；父进程
也会打印 `p N: exec failed` 把资源和管道两种失败分开。

板子 384 KiB 任务区跑 10 个 64 KiB 窗口本就到边，这不是 bug，但测试要按 1~3 对跑。

### 24.7 复现工具

`user/piperace.c`：一对一个写端一个读端，各 128 字节一次，图案按位置生成，读者
逐字节校验。`piperace <pairs> <KiB>` 一行汇总。这是唯一能在真板上稳定复现管道
竞态的装置——`smp` 测的是调度器，`cat | wc` 只能在单核 QEMU 上验字节数精确。

`scripts/rp2350/smptest.py --func` 里保留 `cat readme.txt | wc`。

### 24.8 验收

- 真板 `rp2350smp`：历史结果 功能 7/7；`piperace 1 64` / `2 32` / `3 32` 全 ok。
  `2 64` 的根因已修（§24.4.5）。修复前固件上复现：`piperace 2 64` →
  `p0: w=95201004 r=00000020`、`[E ep: pid 74 gen 33 r=0 w=1 cnt=1920]`。
  烧修复镜像后：单核 `rp2350` `4 64` 10/10（宽判据前）+ 5/5（最终版）；
  `rp2350smp` `2 64` 3/3、`4 64` **15/15**；加 `run spin &` 负载的
  `4 128` **70/70**（含计数探针 50 轮、干净镜像 20 轮）。SMP 的 pended-SVC
  残留已按 §24.4.2 的宽判据修掉。
- QEMU：mps2 单核最终版 `4 64` 5/5；rv32smp（2 hart）`4 64` 10/10；
  x86/rv32/arm/rp2350/rp2350smp 均可构建。
- `rp2350smp` 最终镜像的**长浸泡**：`run spin &` + `piperace 4 128` 40 轮
  40/40；每 10 轮插 `cat demo.py | wc`，四次都是 **2788** 字节；浸泡后
  func 检查 7/7。
- **官方电池等价**（`scripts/rp2350/smptest.py` 的判据，本机无 pyserial，
  用同命令手工复刻）：5×`smp -s 4 500` + 3×`-s 2 500` + 2×`-s 1 500` +
  `smp` 拓扑，**0 失败**；`--func` 7 项（echo/ls/free/ps/cat|wc/py/sleep）
  7/7。
- C3 真机 `accept.py` **24/24**。
- QEMU：x86 全套冒烟 ok；rv32smp（`cpus=2`）`piperace 1 16` ok、`cat demo.py | wc`
  2788 精确。
- QEMU mps2 单核（探针撤除后的干净镜像）：`piperace 4 64` 23/23、
  `3 64` 11/11、`2 64` 12/12、`1 64` 5/5、`4 32` 5/5；`tostest smoke -target arm`
  （hello + `piperace 2 64`）OK。
- `tostest compat -targets x86,rv32` OK；x86 / rv32 / rp2350 / rp2350smp 均可构建。
- 镜像 `e90e21a5…`（与实测镜像 md5 相同）。

### 24.9 SMP 性能基准（smpbench）

`user/smpbench.c` 回答“第二个核到底带来多少”，替代 §24.5 那个不可靠的块计数：

```
smpbench [iters]     # 默认 5,000,000 次/worker，best of 2
```

- **固定工作量**：一个 worker 单独跑 N 次，得 T1；两个 worker 各跑 N 次并行，
  得 T2a/T2b。加速比 = `2*T1 / max(T2a,T2b)`。
- 每个 worker 用 `clock_ms()` 量自己的墙钟；热循环里没有 syscall、锁或输出。
- 启动用每个 worker 一根 pipe 同步：父进程在两个 exec 都返回后才发 go 字节。

2026-11-08 实测（ATK-DNRP2350AM，board 自己的 USB CDC console）：

| 镜像 | N | T1 | dual (T2a + T2b, wall) | speedup |
|---|---|---|---|---|
| `rp2350`（单核） | 5M | 139 ms | 277 + 276 (277) | **1.00x** |
| `rp2350smp` | 5M | 142 ms | 143 + 156 (156) | **1.82x** |
| `rp2350smp` | 10M | 282–284 ms | 282 + 309 (309) | 1.83x |
| `rp2350smp` | 20M | 564 ms | 619 + 559 (619) | 1.82x |
| QEMU mps2（单核） | 20M | 25 ms | 44 + 45 (45) | 1.1x |

大工作量下稳定在 **1.8x**，不是 2.0x：两个 M33 共享 XIP/总线，且其中一个 worker
与 console tick 共享 CPU0（它稳定慢 ~9%）。单核镜像 1.00x、QEMU 单核 ~1.0x，
说明工具没有把时间算成两倍。连续 3 次 5M 都是 1.82x，重复性够用。


## 25. RP2350 RISC-V (Hazard3) 移植

`-target rp2350rv` / `-target rp2350rvsmp` 在**同一块 ATK-DNRP2350AM** 上把
RP2350 切到它的 Hazard3 核：内核从 XIP flash（0x10000000）原地执行，
`.data/.bss` 在 0x20000000 的 SRAM，littlefs 仍在 flash 0x80000，用户程序在
0x20020000 起的 384 KiB 窗口里跑 U-mode。`rp2350rv` 单 hart；`rp2350rvsmp`
把第二个 Hazard3 也拉起来。

新增文件：`board/rp2350rv/{kernel.ld,image_def.S,platform.c,layout.c,
board_devs.c,board_internal.h,rp2350rv_memmap.h}`；驱动位于
`drivers/gpio/rp2350rv/`、`drivers/spi/rp2350rv/`、
`drivers/display/st7789/rp2350rv/`、`drivers/usb/rp2350rv/`（复用 ARM 侧
TinyUSB 栈：`usb_cdc.c/.h`、`tusb_shim.c`、`tusb_config.h`）；
`internal/build/build.go` 多两个 target，
`internal/build/uf2.go` 多一个 `emitRP2350RVUF2`（family **0xe48bff5a**，
IMAGE_TYPE **0x1101** = EXE|CPU_RISCV|CHIP_RP2350，带 ENTRY_POINT item）。
`arch/rv32/start.S` 增加可选的 `.data` 拷贝（`TOS_COPY_DATA`）；ROM 只跳到
入口点，不会搬段。

三个真机坑，都是烧进去才看见的：

1. **不要在未切走 clk_sys 的情况下复位 PLL_SYS。** RISC-V boot ROM 可能把
   clk_sys 留在 PLL_SYS 上；第一版照 ARM 端口的顺序直接 reset PLL，CPU 当场停，
   UART 一个字节都没有。正确顺序是 pico-sdk 的：先把 clk_sys/clk_ref 的 SRC
   清到 glitchless 源并等 SELECTED，再动 PLL。第一版镜像先用不碰 PLL 的方式
   上板（实测 clk_sys ≈ 11.94 MHz），确认入口/PMP/UART 都对之后，第二步补上
   这个顺序：`[A] pre-pll ok` 在 12 MHz 下发出并 flush，随后 PLL_SYS 150 MHz、
   `[B] clk=0x08f0d568`。UART 分频和 mtime 间隔始终来自 FC0 的测量值。
2. **RP2350 的 Hazard3 PMP 不支持 TOR**（A=1 被 WARL 成 OFF）。通用 rv32 层用
   TOR 描述代码/数据区，于是用户程序在窗口基址 0x20020000 立刻 instruction
   access fault。现在每个任务窗口按 2 的幂取整、基址对齐到窗口大小
   （`TOS_PMP_POW2_WINDOWS`，只在 rp2350rv 定义），一个 NAPOT entry 精确覆盖
   该窗口；U-mode 只能碰自己的 code/data/heap/stack，内核、flash、MMIO 和别的
   任务都在外面。窗口内部的 code/data/guard 分界仍不细分（要再分得让镜像的
   `wx` 落在对齐边界上），RP2040 的 flat 模型也是这个粒度。真机上
   `prot stack` 会越界写进 shell 的窗口并被杀，证明逐任务边界生效。
3. **RISC-V 的 `sev` 是 Hazard3 的 `h3.unblock`，不是 fence。** core 1 在 boot
   ROM 里以 `msleep SLEEPONBLOCK|POWERDOWN` 的 `h3.block` 等待；FIFO push 后
   必须执行 `slt x0, x0, x1`（编码上的 unblock），否则握手超时、只有 1/2 CPU
   上线。握手序列是 pico-sdk 的 `{0,0,1,mtvec,sp,_start}`；core 1 从 `_start`
   走 `.Lsecondary`，等 `cpu_release` 后用自己的栈和 `trap_vectors_h1` 进入
   `cpu_secondary_main`。mtime 是两核共享的，但 **mtimecmp 是 core-local**
   （pico-sdk riscv_platform_timer.h），所以每 hart 都能有自己的 1 kHz tick。

USB CDC 使用 ARM 侧同一套 TinyUSB 设备栈，只是把 IRQ 14 改成轮询（Hazard3
外部中断要走自己的 CSR 窗口）：枚举期间 `usb_cdc_console_init()` 的紧循环负责
控制传输，之后 CPU 0 在 console idle/tick 里 `usb_cdc_poll()`。因为 shell 可能
被调度到 CPU 1，输出不能只由 CPU 0 写 FIFO：新增 1 KiB 跨核 TX ring，
`usb_cdc_putc()` 在任意核入队（短 spinlock + 本核关中断），CPU 0 在 poll 里
排空。COM7 上 `help`（约 3 KB）和 `cat demo.py | wc` 在双 spin 负载下都完整。

真机验收（COM6 = UART0，COM7 = USB CDC，115200；clk_sys = 150 MHz；
板子当前烧的是 `rp2350rvsmp`）：

| 项目 | 结果 |
|---|---|
| 启动 | `[A] pre-pll ok` → `[B] clk=0x08f0d568` → `[U] usb hw=1 cfg=1` → `[smp] 2 of 2 CPU(s) online` |
| `free` | SRAM 512 KiB，kernel 108 KiB，tasks 384 KiB，heap 32 KiB |
| `ls` / `df` | 300 KiB littlefs（75 blocks），46 个文件 |
| `hello` | `hello from ring 3!` |
| `cat demo.py \| wc` | 116 / 343 / **2788**（UART 与 USB 都一致） |
| `py pytest.py` | **54 checks, 0 failed** |
| `piperace 4 64` / `4 128` | 4 对全部校验通过；双 spin 负载下 `4 128` 10/10 |
| `dd of=... / dd if=...` | 4 KiB 写入、读回、pattern verified、`rm` 后 df 复原 |
| `reboot` | ROM 正常重启，SMP 重新上线 |
| `smpbench 5000000` | 单 hart 1.00x；双 hart **1.89–2.01x**（single 209–227 ms） |
| `sleep 2000` | 主机计时 2.07 s |
| `tostest compat -targets x86,rv32` | OK |

`plat_now_ns()` 一开始把 mtime 当 150 MHz 算（12 MHz 下 `clock_ms` 快 12.5 倍，
基准看起来像卡死）；改成用实测 `g_clk_hz` 后计时正常。150 MHz 下 `smpbench`
每 5M 次 ~210 ms，与 12 MHz 时的 ~2.5 s 正好差 12.5 倍，说明时钟链和计时都是
一致的。

尚未做：窗口内部 code/data/guard 的细分（需要用户镜像的 writable 起点对齐）。
RP2040 目标当前因 `user/smp.c` 的 Thumb-2 内联汇编无法在 M0+ 编译而构建失败，
与本次移植无关。


## 26. CoreMark 1.0

`user/coremark/` 是 EEMBC CoreMark 1.0（Apache-2.0，`LICENSE.md`）的 TinyOS
port。上游的 5 个 core 文件算法未改，只加了：

- `core_portme.[ch]`：2K performance run（`MEM_STATIC`、`SEED_VOLATILE`）、
  用 `clock_ms()` 计时，`CORE_TICKS` 是毫秒；`HAS_FLOAT 0`，因为 RV32 用户
  程序不链 soft-float。
- `ee_printf.c`：只改 `uart_send_char()` 为 `putchar()`，并把 `coremark.h`
  改成相对 include。
- `core_main.c`：只加一行 `[tos] CoreMark 1.0`，用毫秒 ticks 算分；官方的
  整数秒路径在 10–20 s 的跑分里只有 1 s 分辨率。算法、CRC 和验证逻辑未动。

运行：

```
coremark              # 自动校准到 ~10 s
coremark 6000         # 固定 6000 次，便于比较
```

QEMU rv32 用 `coremark 100` 验证过 CRC（seedcrc 0xe9f5、crclist 0xe714、
crcmatrix 0x1fd7、crcstate 0x8e3a）；`crcfinal` 随迭代数变化，不是固定值。

真板、150 MHz、默认 `-O2`/PIE、TinyOS 抢占 tick 下：

| 镜像 | iterations | ticks (ms) | iter/s | CoreMark/MHz |
|---|---|---|---|---|
| `rp2350`（M33，150 MHz，单 hart） | 11000 | 20875 | **526.9** | **3.51** |
| `rp2350rv`（Hazard3，150 MHz，单 hart） | 6000 | 14628 | **410.2** | **2.73** |
| `esp32c3`（RV32IMC，160 MHz，单 hart） | 2000 | 11749 | **170.2** | **1.06** |
| `rp2350smp`（M33，1 worker） | 6000 | 11825 | 507.4 | 3.38 |
| `rp2350rvsmp`（Hazard3，1 worker） | 6000 | 13604 | 441.1 | 2.94 |

单 hart 绝对吞吐：M33 领先 Hazard3 **1.28×**、领先 C3 **3.10×**；Hazard3 领先
C3 **2.41×**。按每 MHz：3.51 / 2.73 / 1.06，M33 对 C3 是 3.31×。

C3 是 160 MHz 的 RV32IMC 小核（无 A 扩展、无分支预测），绝对分和每 MHz 分都
明显低于 Hazard3；这是核本身的定位差异，不是 TinyOS 开销。

绝对值低于公开的 4.09 / 3.81–4.15 CoreMark/MHz：公开数通常是 `-O3` + 裸机，
而这里是 `-O2` + PIE + 1000 Hz 抢占 + USB 轮询的 TinyOS 用户程序。两边用
**同一套 flags**，所以 1.28× 是同口径结果；和上一节 smpbench 的 1.5× 相比，
CoreMark 的差距更小，符合它比 LCG 循环有更多指令级并行的预期。

试过 `TOS_OPT=-O3`，但整个镜像反而慢约 1.7×（`smpbench` 139→237 ms，
`coremark 6000` 18.7 s），疑似 I-cache/代码布局效应，因此正式数据用 `-O2`。

### 26.1 设备端口

`board/rp2350rv/` 现在与 ARM 端口设备同源（驱动位于 `drivers/.../rp2350rv/`）：

- `drivers/gpio/rp2350rv/gpio.c`：GPIO（30 pin）+ ADC0..3 + 片上温度通道（PLL_SYS/4 = 37.5 MHz）。
- `drivers/spi/rp2350rv/spi.c`/`spi.h`：PL022 轮询 SPI。
- `drivers/display/st7789/rp2350rv/lcd_st7789.c`/`font5x7.h`：ST7789V2 135x240（SPI1 + DMA 通道 10）。
- `board_devs.c`：ATK-DNRP2350AM 的 11 行设备表（led0/key0/lcd0/tf0/uart0/
  usb0/adc0..3/flash0）；TF 卡和 ARM 端口一样只描述、不驱动。
- `board_internal.h`：`plat_led_owned()`，心跳让位给用户。
- `rp2350rv_memmap.h`：补齐 ADC/SPI/DMA/TIMER 的寄存器定义。

`lcd_delay_ms()` 不再读 SysTick（RV32 没有），改为 RP2350 TIMER0 的
1 MHz 自由计数器（`0x400b0028`）。原因和 ARM 端口读 SysTick 下计数器一样：
面板初始化发生在 `plat_irq_enable()` 之前，`g_ticks` 还没开始走。

真机验证（`rp2350rvsmp`）：

| 项目 | 结果 |
|---|---|
| `/proc/dev` | 完整 11 行，`board: ATK-DNRP2350AM (RV32)` |
| `/dev` | `led0 key0 lcd0 screen fb0 gpio adc` |
| `gpio 3 0/1` | 写读一致（蓝色 LED） |
| `echo 0/1 > /dev/led0` | 取得所有权后心跳停止，引脚保持用户值 |
| `cat /dev/key0` | `0`（释放） |
| `adc 0` / `adc 4` | 0 mV / 866 counts ≈ 697 mV（温度） |
| `screen` / `screen text` / `screen dev` | 返回并绘制 |
| `fb info` / `fb demo` | 240x135 RGB565；画完 240x135 位图 |
| `lcdconsole on` | 控制台镜像到面板 |
| 设备加入后 | `smpbench 5M` 1.88×、`piperace 2 64` 通过、`reboot` 正常 |

尚未做：窗口内部 code/data/guard 的细分（需要用户镜像的 writable 起点对齐）；
TF 卡只有设备表描述，没有 SD 驱动（与 ARM 端口一致）。


## 27. /proc/cpuinfo

Linux 风格的 `/proc/cpuinfo`，和其他 proc 文件一样在 `open()` 时生成快照。
每个在线 CPU 一段：

```
processor       : 0
model name      : Hazard3 (RP2350)
isa             : rv32imac
cpu MHz         : 150.000

processor       : 1
...

board           : ATK-DNRP2350AM (RV32)
cpus            : 2
tick Hz         : 1000
```

三个平台钩子（`kernel/platform.h`，`kernel/cpu.c` 给弱默认）：

- `plat_cpu_model()`：板级填“核 + 芯片”，如 `Hazard3 (RP2350)`、
  `Cortex-M33 (RP2350)`、`ESP32-C3`；未知返回 `"unknown"`。
- `plat_cpu_hz()`：板级填实际核心频率（RP2350 用测量后的 `g_clk_hz`，
  C3 用 `plat_apb_hz() * 2`），不知道返回 0，文件里显示 `unknown`。
- `plat_cpu_isa()`：架构层按编译宏填，RV32 用
  `__riscv_atomic`/`__riscv_compressed` 区分 `rv32imac`/`rv32imc`/`rv32im`，
  ARM 用 `__ARM_ARCH_8M_MAIN__`/`7M`/`6M` 区分 `thumbv8m.main`/`thumbv7m`/
  `thumbv6m`，x86 返回 `i686`。
- 每段的数量来自运行时的 `cpu_count`，所以 `rp2350rvsmp` 自动两段。

真机/模拟器实测：

| 目标 | model | isa | cpu MHz | board |
|---|---|---|---|---|
| `rp2350rvsmp` | Hazard3 (RP2350) | rv32imac | 150.000 | ATK-DNRP2350AM (RV32) |
| `rp2350` | Cortex-M33 (RP2350) | thumbv8m.main | 150.000 | ATK-DNRP2350AM |
| `esp32c3` | ESP32-C3 | rv32imc | 160.000 | ESP32-C3 core (CH340K) |
| `rv32` (QEMU) | QEMU virt RISC-V | rv32imc | unknown | QEMU virt (RV32) |
| `arm` (QEMU) | Cortex-M3 (QEMU mps2) | thumbv7m | 25.000 | QEMU mps2-an385 |
| `x86` (QEMU) | i686 (PC) | i686 | unknown | PC (i686) |


## 28. RV32 双核 SMP：四个真 bug 与一套开关式诊断（2026-11-14，已真机验收）

起点是 `rp2350rvsmp`（Hazard3 ×2，150 MHz）跑 `dd` 写 flash 间歇报
`dd: write error at block 0 (-1)`，偶尔连 shell 都起两份。四个独立的 bug，
每一个单独存在时都能装成"偶发"：

1. **RV32 缺 `plat_spin_try_acquire()`**（`arch/rv32/platform.c`）。只有 ARM 实现了
   它，`kernel/cpu.c` 的弱默认是"单核，锁不会争用，直接返回 1"，于是
   `spin_trylock()` 在 RV32 SMP 上恒成功、`proc_lock` 形同虚设。指纹很清楚：
   shell 横幅打两次、`PANIC no runnable process` 前 `[sched] cpu 0 idle:
   c0=1/1 c1=1/1`、`smpbench` 1.00x、`smp` 的 worker 全报 cpu 0。实现用
   `amoswap.w.aq`：它无条件写入 1，失败时锁原样不动，只有返回值说明归属。
2. **park 握手只发布一次 ack**（rp2350 / rp2040 / rp2350rv 三端口同一个错）。
   驻留核在 `rp_flash_park_loop()` 里写 `g_flash_parked = 1` 然后等 flag 落下；
   写者清掉它再等下一次 ack。littlefs 背靠背两次块操作时，驻留核还在循环里、
   不会重新进入，第二个请求等不到 ack，20M 圈后返回 -1 → `dd` 报错。
   改成**代际计数**：`myreq = ++g_park_req`，判据 `g_park_ack == myreq`，
   驻留循环每轮都重新发布 ack（flag 只在 XIP 恢复后才清，所以语义安全）。
3. **`console_write()` 抢不到锁就整体丢弃**（`kernel/console.c`）。
   双核上通常是对面核在 1 ms tick 里排空回显，此时整块写被丢掉：命令行 ≥16 字节
   就没输出、连提示符一起消失（16 字节的回显刚好在程序启动前排完，17 字节排不完）。
   改成记录持锁 CPU：**别的核持锁 → 等，自己持锁（中断重入）→ 丢**。
   `kprintf()` 保留 trylock，诊断行宁可丢也不能把卡死的板子卡得更死。
4. **双调度窗口**（`kernel/process.c`）。`block_locked()` 清 `cpu` 与
   `sched_switch()` 之间，唤醒落在窗口内会把任务重新置 READY 且 `cpu < 0`，
   两个核可能同时选中它 —— 同一个 shell 跑在两套内核栈上，输的那份拿走了
   控制台。`sched_switch()` 现在用 `requeued` 标记识别这种情况，就地恢复自身
   并返回被唤醒者补好的帧，不再走 `sched_pick()`。

顺带修掉的两个"看起来像原因"的问题：

- **SIO 门锁的判定**（`arch/arm/platform.c`）：RP2350 对 `SIO_SPINLOCK0 + 4n` 的
  读返回 one-hot（lock 31 给 `0x80000000`，lock 30 给 `0x40000000`），
  `>> 31` 只在 lock 31 上成立。判"非零"两个芯片都对。
- **写 flash 期间不能关看门狗**。旧代码在 ROM 调用前 `wdt_disable()`，
  一次 wedge 就从"可恢复的错误"变成"永远静止的板子"。现在全程武装只喂不关
  （2 s 重装，一次擦写 ~50 ms），并给 `rom_enter_bootsel()` 加了
  `rom_stop_other_core()`：ROM 引导器只跑 core 0 且事后不复位芯片，
  另一核必须停死，否则新镜像和旧内核共享一套 `.bss`。

### 诊断开关 `TOS_DIAG`

这轮排查靠的是"板子自己留证据"，不是探针。`[features] diag = true`
（`configs/README.md` 有说明，默认关、关掉零成本）打开后：

- 每核 tick 心跳 + 三个"现在在不在里面"的标志（屏蔽区 / 自旋 / park），
  健康核能判断对面是**卡在屏蔽段里**还是**还在周期性地进临界区**；
- 屏蔽点记 site id + 调用者 LR（`TOS_MASKSITE_*`），
  `addr2line -e build/kernel-<target>.elf <lr>` 直接落到源码行；
- 全部放 `.noinit`（链接脚本在 `.bss` 之后），看门狗复位不清 SRAM，
  下次启动第一行打 `[PREV] ...`；RP2350 另有 `WATCHDOG_SCRATCH0..7`
  记 flash 序列走到了哪一步（只有上电才清）。

两条方法学上的教训，写在这里免得下次再踩：**抓冷启动必须先挂好读线程再按
RESET**（CH343 不随 MCU 复位掉线，第一个字节抓得到）；**压力测试走 CDC 不走
CH343**（约 18 轮 dd 之后 UART 的 host→MCU 方向会单向死掉，探针显示
`cr`/`pad`/`funcsel` 全正常而 RX FIFO 恒空）。另外 `-DTOS_DIAG` 本身会改变
时序，A/B 结论要在同一个固件里用运行期开关复核。

### 验收（`rp2350rvsmp` 真机，ATK-DNRP2350AM）

| 项目 | 结果 |
|---|---|
| `dd of=/big.bin bs=4096 count=1` × 12 轮（写 + 读校验，CDC） | 0 失败 0 重启 |
| 单扇区写累计 200+60+30+40+20 轮（诊断版固件，同一天） | 0 失败 0 重启 |
| `smpbench`（关闭诊断的提交版） | **1.97x**：single 206 ms，dual 205 + 209 ms |
| `smp -s 2 300` | worker 0 → cpu 0，worker 1 → cpu 1，各 5197 / 5243 blocks |
| 16 字符命令行 ×18 次 | 0 丢（修前 10 次丢 9 次） |
| 11 个目标构建 | 全 OK；`diag = true` 的 rp2350 / rp2350smp / rp2350rvsmp 也能编译 |

未验证：`rp2040smp` 只过构建（本机没有 rp2040 板），它的 park 握手是同一份修复。
`threadbench` 的 ratio 与 SMP 性能数字在 QEMU 里没有意义，别用它们下结论。


## 建议的讨论顺序

1. ~~单文件多入口~~（§6 结论已进 §7.1，等拍板要不要实现）。
2. ~~段头表裁剪~~（2026-11-06 已做，§16；+12 KiB littlefs 可用空间）。
3. **控制台输出锁**（§16 上半）：需要布局稳定的实现，别直接改 `console.c` /
   `process.c` 就发版，先解决 `priotest` 的相位敏感性。注意 §16 的归因修正：
   地址搬动本身不必然翻相位，改完仍要做真机 A/B。
4. ~~`script` 先合、`sched-prio` 后 rebase~~（2026-11-02 已按此顺序完成）。
5. ~~B 路线第二步：输入侧分块 + 输出直写窗口~~（2026-11-06 深夜已做，§18；
   C3 堆 56 → 16 KiB，堆峰值 28 424 → 768 B）。
6. **RP2040 的 RAM 布局**（§19）：内核窗口写着 128 KiB、实际只用 77.6 KiB，
   50 KiB 白白占着；把 `PLAT_KERNEL_MAX` 下移 + 缩堆/任务表/内核栈，能把用户区
   从 136 KiB 抬到 190 KiB 以上。同一套动作已在 C3 上完整验证（§20：
   内核 79 088 → 49 760 B、任务区 15 → 19 窗口、24/24 验收通过），
   所以 RP2040 上要验的只剩 ARM 侧的 8 KiB 内核栈。仍要先攒一台可烧的 Pico。
7. **XIP** 只在「真要跑 64 KiB 以上的程序」时回到桌上，先做 `irom.S` 探针。
8. **最小 shell 进不进 init 位**（§21）：`bin/msh` 已实测一个 16 KiB 窗口够用
   （rv32 5 792 B 镜像、余 2 400），但谁叫 `bin/sh` 会牵动脚本、文档和验收，先拍板。
