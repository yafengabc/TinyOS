# 待决清单

每次评估完把结论钉在这里：**已落地的写进「已合并」，被否的写清理由**（省得下次重提）。
体积数字都是 `-Oz`、与 `tos build` 逐字节一致的构建测出来的。

六件：三件已落地（`reboot`、QEMU 慢的定性、优先级与脚本已于 2026-11-02 合并）、
两件只评估（XIP 的结论是「能做但不该做」，SO/多入口 的结论是「多入口值得做」）。

| # | 事项 | 结论 | 代价 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | `reboot` 真复位 | 做，五端口全实现 | 未单独计量 | 已合并 `fc08eb2`（C3 20/20） |
| 2 | QEMU memcpy 慢 / RV32EMU 复现 | 不是 bug，是 TCG 每迭代重译 | 无（只影响写法） | 已记录 `b31e854` + §8 |
| 3 | 调度器加优先级 | 值得做，三级 + 可抢占 | x86 `-O2` `.text` +512、rv32 `-Oz` `.text` ±0 | **已合并** `97b07c4` |
| 4 | 大程序 XIP | **能做，但现在不该做** | 见下（四条硬约束） | 只评估 |
| 5 | 极简 shell 脚本 | 值得做，`source` + 启动脚本 | rv32 sh +576 B 原始 / +703 B 压缩 | **已合并** `7d949a3` |
| 6 | 单文件多入口 vs 共享库 | 多入口 **−55%**，SO **+18%** | 内核 +~50 B | 只评估，已写进 §7.1（`d681089`） |
| 7 | 每程序栈（默认 8 KiB + `TOS_STACK`） | 做，ELF note 通道，PMP 规则数不变 | 内核 +~60 行；小工具窗口 32→16 KiB | **已合并**（`PLATFORM-DESIGN.md` §8.9.1） |
| 8 | 每程序堆（`TOS_HEAP`，note type=2） | 做，与栈同一个通道，无 2 的幂约束（TOR） | 窗口公式 + 堆一项；脚本引擎直接受益 | **已合并**（`PLATFORM-DESIGN.md` §8.9.3） |
| 9 | 控制台传文件（`tosput` + `rx`） | 做，行协议 + 停等 + 板端 CRC32，零内核改动 | `tools/tosput` + `user/rx.c` | **已合并**（`docs/FILE-TRANSFER.md`） |
| 10 | 串口 RX 突发上限 ~128 B/tick | **原诊断错了**：UART0 接收早已中断驱动；真凶是 `usj_putc` 无 USB 主机时每字节空转超时，而内核打印关中断，把 UART0 排空一起饿死 | 已改成 USJ 发送环 4 KiB + IN_EMPTY 中断、USJ 接收中断、raw 环 1 KiB（按实测缩回 512，见下）、中断里唤醒读者 | **已合并**；吞吐 1.0 → 7.0 KiB/s（理论 8.6）。原生 USB 两路中断未上真机（本机只插 CH343），见 §10 |
| 11 | 传文件窗口化（不等 ACK 连发 N 块） | 未评估：当前 7.0 KiB/s 已是 115200+base64 的 81%，窗口化只能再挤 ~19% | 协议 + 板端乱序/重排逻辑 | 待评估（优先级低） |

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

## 4. 大程序 XIP（结论：不该做）

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

**待定**：要不要花半天写 `irom.S` 探针，先钉死「cache 到底能不能从 kmain 拉起」？
这是唯一能让这条线继续往下走的实验，成本低、结论二元。

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
- **arm(qemu-mps2) 首次 exec 就 HardFault**：`bx lr` 处崩、帧没写回。怀疑 `arm_resume`
  的 FR_HW 分支，或 MPU 区域 0 覆盖整窗且 XN。老 bug，没修——ARM 上的功能验证因此
  只能靠「脚本抓 `init process exited`」兜着，别信绿。
  （2026-11-02 复核：在 `effaa9c` 上原样复现，与 SMP/时钟/外设/管道这一轮无关。
  `ls` 和 `cat` 都会崩，所以是「任何碰 littlefs 的用户态程序」都中招，不只是 ls。）
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

## 建议的讨论顺序

1. ~~单文件多入口~~（§6 结论已进 §7.1，等拍板要不要实现）。
2. **段头表裁剪**：正交、十行改动、−12.4%，谁都不挡——下一个最便宜的动作。
3. ~~`script` 先合、`sched-prio` 后 rebase~~（2026-11-02 已按此顺序完成）。
4. **XIP** 只在「真要跑 64 KiB 以上的程序」时回到桌上，先做 `irom.S` 探针。
