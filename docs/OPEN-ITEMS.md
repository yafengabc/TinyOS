# 待办（只列**现在**还开着的）

这里只放还没做完的事。已关闭的条目——包括**结论和关掉它的那次实测**——都在
[`HISTORY.md`](HISTORY.md) 里，编号沿用原样（代码注释里写的"OPEN-ITEMS §23.3"之类的
引用，去 HISTORY 里找）。

## 1. RP2040 验收表里还差的一项（2026-11-16）

`HISTORY.md` §30.3 那张表里的**窗口数字**没量：`ps` 里 `msh` 的 `wink` 应为 16 KiB、
`sleep` 之类的任务也应是 16 KiB（而不是 24/32），`deep 3` 要打印
`stack grown to 8 KiB` 且 `journal intact`，`deep 6` 要按"先长到 8K → `guard word` →
`no room to grow it; killing`"走。

RP2040 其余部分已经收口（启动、PIO 回环、外设、littlefs、1000 轮 `dd`+`rm`，见
HISTORY §14/§32.3/§35），所以这是这一块板**唯一**还没记账的判据。

## 2. PIO 的"眼睛两项"（RP2040 / RP2350）

汇编和回环都是自动判定、已通过；需要人看的两项还没做：

- `blink` / `blink3`：GP25（Pico）或 GP3（ATK-DNRP2350AM）约 2 Hz 闪烁；
- `ws2812`：GP4 接灯带，`pio tx 0 0 0xff0000` 后首像素亮红。

注意 RP2350 那次"LED 没闪"是**引脚假设**问题（pad 在 2 Hz 翻转，见 HISTORY
§32.2），不是 PIO 的 bug；换板子先确认用户 LED 到底在哪个 GPIO。

## 3. Cortex-M33 的 `deep 6`（设计取舍）

`HISTORY.md` §33.2.1：异常入口需要栈下面有可写的着陆区，护栏洞里只留了 2560 B，
所以 `deep 6`（12 KiB 越界）越过着陆区就 MSTKERR，硬件根本不给异常帧。三条路：
把着陆区扩到 4 KiB（`prot heap` 就守不住）、教增长路径处理"帧没建成"、或者用
`-fstack-limit-*` 之类的编译期预检。选择之前先问"有没有程序真的需要 12 KiB 的栈"。

## 4. 构建期的窗口几何还停在 16 KiB 时代（§36）

`internal/build/build.go::WindowGeometry()` 返回 16 KiB grain / 8 KiB stack /
4 KiB guard，而四块板早就是 4K/4K/4K。它只影响 `checkWindow()` 对
`TOS_WINDOW()`/`TOS_ONE_GRAIN()` 的校验（不影响运行时），所以 `msh` 的
`TOS_ONE_GRAIN()` 其实名不副实。

修法两行：把返回值改成 4K/4K/4K，把 `msh` 的注解换成 `TOS_WINDOW(16*1024)`；改完
重新确认 `msh`/`sh`/`py` 的承诺仍然成立。单独一次提交，别和别的混。

## 5. x86 从来没在 CI 里构建过（§37）

发布矩阵里没有 `x86`，所以它在发行版工具链上直接失败：

```
ld.lld: error: relocation R_386_16 cannot be used against local symbol;
               recompile with -fPIC
```

i386 的 PIE 链接不接受端口生成的 16 位绝对重定位；本机 MSYS2 的 lld 容忍它们。
三条路：给 x86 用户程序换非 PIE 装载、把这几处重定位改成 32 位、或者
`-Wl,-z,notext`（治标）。在那之前 CI 的 `qemu smoke` 只覆盖
`rv32`/`rv32smp`/`arm`。

## 6. goc32：工具链已自包含，剩下前端缺口

`goc32` 不再调用 clang/ld.lld：汇编器在 `goc32/internal/as`，链接器在
`goc32/internal/ld`，两者都已用 QEMU 上的 RV32 / Cortex-M3 程序验证过，并且
**生成的每一条非跳转指令都和 clang 汇编器逐字节比对过**（RV32 3,378 条 / Thumb
3,273 条）。native32 的 ARM 后自增问题（`*s++ = c` 丢旧指针，`gmsh` 的
`echo hi` 崩）已经修好，`gmsh` 在 Cortex-M3 上能跑 `echo hi` / `ls` / `free` /
`ps`。

**目标码大小不是 goc32 的目标**：发布镜像和预编译的 `msh` 仍由 clang 构建；
goc32 的定位是不装 clang 时的单文件开发循环（`goc32` + `tosput/rx`）。所以
RV32 上 goc32 版 `msh` 需要 20 KiB 窗口这件事，不需要去追平 clang 的 16 KiB。

功能宏已经跟着构建走：`tos build` 在 `build/build-config-<target>.txt` 里记下
用户程序的 `-D` 列表，`goc32` 自动导入（`TOS_THREADS`、`TOS_MAILBOX`、
`TOS_<FEATURE>`），并在启用时一起链接 `thread.c`/`thread_switch.S` 或
`kthread.c`。用 `-config` 打开 threads/mailbox/net 构建后，`goc32` 编出的
`gthread`（threadtest）和 `gmail`（mailtest）在 QEMU 上跑通；`-no-build-defines`
可以关掉自动导入。


还剩前端缺口：浮点 IR 和多数 GCC 扩展 asm，在代码生成之前就被拒绝。132 次
`user/*.c` 构建里有 111 次能走完整条流水线；其余是浮点/soft-float、扩展 asm，
以及没有 `main` 的库编译单元（`lib.c`、`syscall.c`、`thread.c` 等），不是
汇编/链接失败。`for` 初始化里的声明和带 clobber 的空 asm 屏障已经支持。

## 7. 明确"先不做"的设计项（有理由，别当遗忘）

- **PMP region 位图分配**（`ffs(~mask)`）：Hazard3 的 8..10/11..15 是硬连接线，
  今天实现就是死代码，等第二个从 bootloader 手里接管 PMP 的平台（HISTORY §34）；
- **内核工作队列 / 优先级继承**：现在只有一个延迟用户（USB 轮询）和很短的控制台/
  fs 临界区，出现"整文件拷贝期间持锁"这类真实需求再说；
- **MTD / PSRAM**：`plat_block_*` 够用，接第二种存储时再引入；
- **`connected/disconnected`（`-ENOTCONN`）**：按原话不成立——控制台是 UART+CDC
  复合体，UART 永远"连着"；状态已经能从 `/proc/cdcfix` 看到。

## 8. 文档与博客

- `README.md` / `README.zh-CN.md` 是入口，长手册在 `docs/GUIDE.zh-CN.md`；
- 计划中的系列文章（设计取舍、每一次真机事故的复盘）放在 `docs/blog/`，目前还没开始写。
