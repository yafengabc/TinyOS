# PIO（RP2040）

RP2040 的两个 PIO 块各带 4 个状态机、32 条 16 位指令的指令存储和 4 字深的
TX/RX FIFO。它的"程序"不是代码段，而是**写进 INSTR_MEM 的 32 个 16 位字**，
外加一组不在指令字里的配置：wrap 地址、side-set 宽度/语义、时钟分频、引脚窗口、
移位方向与 autopull/autopush。

TinyOS 把这件事拆成两层：

| 位置 | 作用 |
| --- | --- |
| `board/rp2040/pio.c` | 内核 HAL：校验、装载指令、配置 SM、启停、FIFO、引脚 mux、进程退出清理 |
| `user/pio.c` | 用户态汇编器 + 控制台命令（`pio asm/start/stop/tx/rx/rel`） |
| `files/pio/*.pio` | 随镜像打包的示例程序 |
| `SYS_PIO` / `struct pio_req` | 唯一的 ABI，用户程序和内核之间只传这个结构 |

menuconfig 的 `pio` 特性默认在 rp2040 上打开；关掉时 HAL 和命令一起消失。

## 控制台用法

```sh
pio asm /pio/loopback.pio 0 0   # 汇编、打印每个字、装入 PIO0 SM0
pio start 0 0
pio tx 0 0 1 0 1 1              # 写 TX FIFO（32 位字）
pio rx 0 0 4                    # 读 RX FIFO，打印十六进制
pio stop 0 0
pio rel 0 0                     # 停 SM 并把引脚交回 GPIO
pio regs 0 0                    # 读回 SM 寄存器（诊断用，只读）
```

`pio regs` 打印 11 个字 `ctrl / fstat / execctrl / shiftctrl / pinctrl / clkdiv /
addr / owner / gpio_ctrl / pad / flevel`，再加一行解码后的引脚状态
（`funcsel/ie/od/iso`）和一行 FIFO 层数；`pio exec <block> <sm> <word>` 把一条
指令直接写进 `SMx_INSTR` 立即执行，用来在没有程序的情况下验证引脚通路。
八个字。装载器的"意图"和寄存器里的内容是两件事——位域写偏了会写到一个看起来也
合理的值上，只有读回来才能和手册对上；`addr` 还能看出 SM 是不是卡在 `pull`。

`loopback.pio` 用一个引脚同时做 OUT 和 IN（GP4），不需要跳线：`pio tx` 的 bit0
被驱动到 GP4，`in pins,1` 再从 pad 读回来。`blink.pio` 用两层循环和 delay 让
GP25 以约 2 Hz 翻转。`ws2812.pio` 是一条 WS2812 数据线：side-set 打高/低脉冲，
delay 定 800 kHz 的位宽，autopull 从 TX FIFO 逐位取数。

## 汇编语法

注释 `;` 或 `//`；标签是 `name:`；数字支持十进制、`0x`、`0b`。

| 指令 | 写法 |
| --- | --- |
| `jmp` | `jmp [cond,] label`，cond: `always` `!x` `x--` `!y` `y--` `x!=y` `pin` `!osre` |
| `wait` | `wait POL SRC, IDX [rel]`，SRC: `gpio` `pin` `irq` |
| `in` | `in SRC, COUNT`，SRC: `pins` `x` `y` `null` `isr` `osr` |
| `out` | `out DEST, COUNT`，DEST: `pins` `x` `y` `null` `pindirs` `pc` `isr` `exec` |
| `push` | `push [iffull] [block\|noblock]` |
| `pull` | `pull [ifempty] [block\|noblock]` |
| `mov` | `mov DEST, [invert\|reverse] SRC` |
| `irq` | `irq [wait\|clear] IDX [rel]` |
| `set` | `set DEST, VALUE`，DEST: `pins` `x` `y` `pindirs`，VALUE 0..31 |
| `nop` | `mov y, y` |

任何指令都可以带 `side N` 和/或 `[DELAY]`，二者共享指令字的 5 位 delay/side-set
字段：side-set 值占高位，delay 占低 `5 - side_set 位数` 位；`.side_set N opt` 时
side-set 字段的最高位是"本拍驱动引脚"的使能位，值本身少一位。`COUNT` 为 32 时
编码成 0。

指令：

| 指令 | 作用 |
| --- | --- |
| `.program NAME` | 名字，仅供阅读 |
| `.side_set N [opt] [pindirs]` | side-set 位数与语义 |
| `.origin N` | 程序在 INSTR_MEM 里的起始槽位（必须写在代码前） |
| `.pin BASE COUNT` | 引脚窗口；OUT/SET/SIDESET/IN 的 base 都用它 |
| `.clkdiv INT [FRAC]` | SM 时钟分频，125 MHz / (INT + FRAC/256) |
| `.shiftctrl HEX` | 直接写 SHIFTCTRL（autopull/autopush、阈值、移位方向） |
| `.wrap_target` / `.wrap` | 写 EXECCTRL 的 wrap 地址 |
| `.word HEX` | 直接放一个字（数据或手写编码） |

## ABI

```c
struct pio_req {
    int op;            /* PIO_LOAD/START/STOP/TX/RX/RELEASE */
    int block, sm;     /* 0..1, 0..3 */
    const void *buf;   /* u16 指令字（LOAD）或 u32 数据（TX/RX） */
    int len;           /* 字数 */
    int origin, wrap_top, wrap_bottom;
    int side_set, flags;      /* PIO_F_SIDE_OPT / PIO_F_SIDE_PINDIR */
    int clkdiv_int, clkdiv_frac;
    int pin_base, pin_count;  /* -1 = 不动引脚 */
    int shiftctrl;
};
```

`LOAD` 会先停 SM、清内部状态和两个 FIFO、写指令存储、写
`CLKDIV/EXECCTRL/SHIFTCTRL/PINCTRL`、mux 引脚，但不自动启动；`START` 单独做。
`TX`/`RX` 是非阻塞的字操作，返回实际搬运的字数。

内核校验：指令数 ≤ 32、`origin+len ≤ 32`、wrap 落在加载区间内、SM 未被别的
任务占用、引脚窗口在 GP0..GP29 内。

## 生命周期

- **归属进程**：SM 记录加载它的 pid。进程退出时 HAL 停掉它、把引脚 mux 回
  GPIO/SIO（`plat_pio_release`）。不这么做的话，一个跑完就退出的程序会把
  GP0/GP1 从控制台 UART 抢走。
- **重启会丢**：指令存储和 SM 配置都是易失寄存器，芯片复位/重烧后全没了。
  想"开机就有"，把程序存成数组或 `.pio` 文件，在启动时重新 `LOAD`（这正是
  嵌入数组 + 启动装载的用途）。
- 进程活着但 SM 停了，状态仍在；`rel` 才是显式归还。

## 限制与后续

- 汇编器没有 `.define` 和表达式，只支持数字/标签；`.side_set` 的 `opt`/`pindirs`
  已支持，`pio asm` 会把编码后的字逐条打印出来核对。
- 一个 `LOAD` 只配置一个 SM；同一块里用 `.origin` 放多个程序是支持的，由调用者
  负责不重叠。
- 没有把 PIO IRQ 接到内核中断，`irq` 指令只改 PIO 的 IRQ 标志；需要时在用户态
  轮询 `irq` 或后续加中断。
- `INSTR_MEM0..31` 是只写的（四个读口全给状态机），`pio` 回读永远 0，看起来和
  "没装进去"一样；要确认程序内容只能靠执行（`pio exec` 注入 `jmp N` 再读引脚）。
  当前执行位置看 `pio regs` 的 `addr`。
- `SHIFTCTRL` 的两个移位方向位默认是"向右"（复位值 `0x000c0000`），驱动会保留它们；
  写 0 会让 `out pins` 变成先出 MSB。
- autopull/autopush 目前用 `.shiftctrl` 的原始位配置，没有 `.autopull 32` 这类糖。

## 验证

真机验收按 `docs/HW-ACCEPTANCE.md` 的关卡 B–D 走，`scripts/rp2350/pio_accept.py`
一条命令跑完；详细记录在 `docs/HISTORY.md` §32 / §43。

- **`rp2350`（ARM）17/17、`rp2350rvsmp`（RV32）17/17**（2026-11-15）；
- **`rp2040` 17/17**（2026-11-17），日志 `build/pio-rp2040-final-1117e.log`；
  GP25 板载 LED 目视约 2 Hz；WS2812 因没有灯带，视觉项仍未验；
- `loopback.pio`：`pio asm` → `start` → `tx 0 0 1 0 1 1` → `rx 0 0 4`，读回
  `80000000 00000000 80000000 80000000`——一位的 `in pins` 落在 ISR 的 bit31
  （右移），而且紧跟 `out pins` 之后读到的就是同一拍的电平，没有延迟；
- `blink.pio` / `blink3.pio`：RP2350 GP3、RP2040 GP25 都已目视约 2 Hz；脚本的
  `--visual` 才会问，非视觉运行只报"装入成功"；
- `ws2812.pio`：编码与 pico-sdk `pioasm` 生成的 `ws2812.pio.h` 逐字一致
  （`6221 1123 1400 a442`）；实机接一条灯带，`pio tx` 写 `pixel_grb << 8` 看颜色。
  两块板都没有灯带，**视觉项仍未验**。

RP2040 的验收脚本曾把 `blink`（PIO1 SM1）和 `ws2812`（PIO1 SM3）都装在 origin
0：同一 block 的四个 SM 共用 32 字指令存储，后装的 4 字覆盖了 blink 的前 4 条，
`pio start` 于是停在旧 PC、LED 不亮。脚本已把 RP2040 的 ws2812 移到 PIO0 SM3。

真机上踩过、以后改驱动前先看的三条：`SHIFTCTRL` 别写 0（会变成左移，`out pins`
出 MSB）；清 FIFO 的 `FJOIN_RX` 位在 RP2350 是 bit31、RP2040 是 bit14；
`SM_EXECCTRL` 的 wrap 字段在两颗芯片上都是 `[16:12]`/`[11:7]`。
