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
另外有一条正交的零架构改动可以先做：段头表 240 B/文件、`kernel/elf.c`
从不读 `e_shoff`，清零 + 截断即省 **−12.1%**。

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
4. **控制台**：`board/rp2040/usb_cdc.c` 是手写 CDC（RP2350 后来换成 TinyUSB 了），
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

## 建议的讨论顺序

1. ~~单文件多入口~~（§6 结论已进 §7.1，等拍板要不要实现）。
2. **段头表裁剪**：正交、十行改动、−12.4%，谁都不挡——下一个最便宜的动作。
3. ~~`script` 先合、`sched-prio` 后 rebase~~（2026-11-02 已按此顺序完成）。
4. **XIP** 只在「真要跑 64 KiB 以上的程序」时回到桌上，先做 `irom.S` 探针。
