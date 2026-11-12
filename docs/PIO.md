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
```

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
- `INSTR_MEM` 是只写的，`pio` 命令不能回读已装入的程序；要看当前执行位置得
  读 `SM_ADDR`（还没暴露）。
- autopull/autopush 目前用 `.shiftctrl` 的原始位配置，没有 `.autopull 32` 这类糖。

## 验证

- `loopback.pio`：`pio asm`、`start`、`tx 1 0 1 1`、`rx 4`，读回应逐位一致；
- `blink.pio`：GP25 约 2 Hz，示波器看；
- `ws2812.pio`：编码与 pico-sdk `pioasm` 生成的 `ws2812.pio.h` 逐字一致
  （`6221 1123 1400 a442`）；实机接一条灯带，`pio tx` 写 `pixel_grb << 8` 看颜色。
