# UART0 发送端自检：RP2350 / Pico 2（2026-11-20）

```text
commit:            待填（本轮 UART 诊断节点本身）
change:            新增 /proc/uart0、/proc/uart0tx、/proc/gp0drive 三个自检口
baseline:          none
target/config:     rpi-pico2-rv32-smp / defaults
board/port/image:  Raspberry Pi Pico 2（RV32 SMP）+ CH343 适配器，CDC=COM27, UART0=COM19
toolchain:         go 1.x, clang 23.1.4, python 3.13
host:              Windows 11 / EKSOFT
T0:                刷机前，UART0 收方向正常（发 uptime 走 UART0，回显从 CDC 出来）
T1:                BOOTSEL 刷机后，UART0 无任何字节（0 字节 / 多次），CDC 正常
T2:                /proc/uart0：UART 编程全部正确
T3:                /proc/uart0tx：GP0 空闲为低，发送时只有微弱毛刺
T4:                /proc/gp0drive：改由 SIO 驱动 GP0 拉高，仍读回低
T5:                对照组 gp1（适配器 TX）与 gp25（LED）读回高，证明回读链路本身正常
logs:              not kept（原始串口输出摘录在下方）
notes:             结论是"板侧寄存器正确、引脚拉不高"，故障在 GP0 网络上，不是固件
```

## 起因

`docs/OPEN-ITEMS.md` 第 5 条：BOOTSEL 刷机后的那一轮起，UART0 连续 **0/20** 轮无输出，
CDC **20/20** 正常，暖重启也救不回来。原假设是「ROM 进 BOOTSEL 时改了 UART pad/时钟，
而 `plat_init` 没重做初始化」——本轮把它验伪了。

## 执行的命令与结果

```sh
# 1. 收方向是否还活着：在 UART0 上发命令，看 CDC 是否有回显
python -c "..."        # u.write(b'uptime\r'); 读 CDC
#   -> CDC 侧收到完整 'uptime: 7641.223 s ...'，UART0 侧 0 字节
#      即 UART0 的 RX 与时钟分频完全正常，坏的是 TX

# 2. 读出 UART 的实际编程（/proc/uart0，RV32 SMP 镜像）
cat /proc/uart0
#   cr=00000301 uarten=1 txe=1 rxe=1
#   fr=00000197 txff=0 txfe=1 busy=0 rxfe=1
#   ibrd=00000051 fbrd=00000018   -> 150MHz / (16 * 81.375) = 115207 Bd
#   lcr_h=00000070 fen=1 wlen=3
#   tx gpio=00000002 pad=00000056 (iso=0 od=0 ie=1)
#   rx gpio=00000002 pad=00000056 (iso=0 od=0 ie=1)
#   clk_peri ctrl=10000800 div=00010000

# 3. 让 UART 发 32 字节 0x55，同时用 SIO_GPIO_IN 采样 GP0
cat /proc/uart0tx
#   idle gp0   low=2048 high=0     <- 空闲本应为高
#   tx   gp0   low=65492 high=44 transitions=88
#   controls   gp1(adapter tx) high=65536  gp25(led) high=65536

# 4. 绕过 UART，直接用 SIO 驱动 GP0 拉高再读回
cat /proc/gp0drive
#   driving 1  high=0 low=2048
#   driving 0  high=0 low=2048
#   writes landed pin_mux=00000005 sio_oe=00000009   <- 写确实落到了寄存器
```

## 关键输出

```text
== 收方向（UART0 发，CDC 回）
CDC   side : b'uptime\r\nuptime: 7641.223 s (7641223 ms)  tick=1000 Hz  cpus=2 ...'
UART0 side : b''

== /proc/gp0drive
gp0drive: pad driven from SIO (uart line briefly off)
  driving 1    high=0 low=2048
  driving 0    high=0 low=2048
  writes landed pin_mux=00000005 sio_oe=00000009 (want 5 / bit0)
  verdict      the line stays low even when the pad drives 1

== /proc/uart0tx
  idle gp0     low=2048 high=0   <- must be high, a UART idles high
  tx   gp0     low=65492 high=44 transitions=88
  controls     gp1(adapter tx) high=65536  gp25(led) high=65536
```

## 结论

1. **不是固件问题。** UART 的 `CR/IBRD/FBRD/LCR_H` 与正常启动逐项一致（UARTEN|TXE|RXE，
   分频 81.375 → 115200，FEN+8N1），引脚 mux 是 UART（FUNCSEL=2），焊盘 `ISO=0 / OD=0 / IE=1`，
   和 RX 侧（工作正常的那一侧）字节级相同。
2. **板子确实在往 DR 写字节。** 因为 UART0 的 RX 一收到命令，CDC 就有回显——回显走的是
   同一条 `plat_console_putc()`；`FR.txff=0 txfe=1` 说明 FIFO 被排空了。
3. **GP0 这根线拉不高。** 换由 SIO 直接驱动（`pin_mux=5`、`sio_oe` bit0 都确认写进去了）
   仍然 2048/2048 采到低电平；而同一时刻 gp1（适配器 TX）、gp25（LED）都稳定读到高，
   说明回读链路和整个 bank 是好的。
4. 所以故障在 **GP0 网络上**：要么线被短到地/被重负载拉低（跳线错位、接到 GND、
   适配器 RX 端有强下拉），要么这颗 pad 的输出级损坏。前者概率高得多——这也解释了
   「20/20 → 0/20」为什么和刷机同步发生：刷机时插拔线束碰到了这根线。
5. **原记录的根因假设（ROM 改了 pad/时钟）验伪**：把 `plat_init` 的 UART 初始化重做一遍
   不会改变任何东西，寄存器本来就已经是对的。

## 需要人工做的两件事（2 分钟）

- 拔掉 CH343 的 GP0 那根线，再 `cat /proc/gp0drive`：
  若这时 `driving 1` 读到高 → 负载在适配器侧（换线/换适配器）；
  若仍全低 → GP0 这颗 pad 输出级坏（换板）。
- 断电后用万用表量 GP0 对 GND 的电阻：正常应远大于 10 kΩ。

## 没跑的

- `rpi-pico`（RP2040）与 `rpi-pico2-smp`（ARM）上的同三个节点：代码已同构实现并编过，
  但本轮板子只插了 Pico 2 的 RV32 SMP 镜像，**NOT RUN**。
- 冷启动/拔电后复测：需要人工拔电（见 `docs/OPEN-ITEMS.md` 第 5 条）。

## 走错的两步（留给下次）

- 第一版 `plat_uart_tx_probe` 在写完 DR 后立刻密集采样，取样窗口整个落在**起始位**上，
  于是健康引脚也读成 "low=1533 high=3"，差点被当成"pad 没驱动"。采样必须跨越整个 87 µs 帧。
- 用 `IO_BANK0.GPIO0_STATUS` 的 bit17 当 `INFROMPAD` 解，读回 `0x2000/0x2200`，
  与本芯片的实际布局对不上。换成 `SIO_GPIO_IN` 后不再依赖任何 status 位布局，
  并且可以顺带用 GP1/GP25 当对照脚。
