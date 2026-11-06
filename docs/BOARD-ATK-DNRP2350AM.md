# ATK-DNRP2350AM 板级外设抽象

本文记录 TinyOS 对正点原子 DNRP2350AM（RP2350A 核心板 + 1.14 寸 SPI 屏）的
外设建模方式：板上有哪些设备、各自挂在哪根脚、内核如何把它们变成 `/dev` 节点、
用户态如何用。真机验证过的结论都标了「实测」。

## 1. 为什么有一层「设备」而不是只有 GPIO

`kernel/periph.c` 只抽象了每个 MCU 都有的两样东西：一根引脚、一个 ADC 通道。
一块板子还有引脚描述不了的东西——一块有分辨率的屏、一个要消抖的按键、一个卡座。
这些东西如果都塞进 GPIO 接口，用户程序就得自己记住「3 号脚是 LED 且低电平点亮」，
换一块板全错。

所以板子用**一张表**描述自己：

```c
typedef struct board_dev {
    const char *name;    /* "led0" —— /dev 下的名字                     */
    const char *model;   /* "ST7789V2 135x240" —— 给人看的型号          */
    uint8_t     cls;     /* TOS_DEV_LED / _KEY / _DISPLAY / ...          */
    uint8_t     flags;   /* TOS_DEV_ACTIVE_LOW / _BOUND / _SHARED        */
    int16_t     pin;     /* 主引脚，-1 表示没有                          */
    int16_t     aux;     /* 类相关的附加信息（总线号、通道号、第二引脚）*/
    const char *detail;  /* 接线说明                                    */
} board_dev_t;
```

`kernel/hwdev.c` 把表里的 LED / KEY / DISPLAY 行变成 `/dev/led0`、`/dev/key0`、
`/dev/lcd0`，其余行只出现在 `/proc/dev` 里（诚实地说明硬件存在，即使还没有驱动）。
`/proc/dev` 是给终端看的宽表（型号 + 接线），屏上用 `LCD_OP_DEVS` 由内核画紧凑版
（每行 `name class pin 接线`，超出 40 列截断）——两种排版同源于一张表。
板子只实现**按类**的钩子（`plat_led_set`、`plat_display_cell` …），不实现按设备的。

极性、消抖这类「板子的事实」放在板级文件里，内核和用户态只说「亮 / 不亮」。

```
用户态             cat /dev/key0        echo 1 > /dev/led0      screen text hi
                     |                      |                       |
内核 class 层      plat_key_read()     plat_led_set()        screen_putc()
                     |                      |                       |
板级 BSP           GPIO2 上拉读         GPIO3 取反驱动         SPI1 + ST7789
```

## 2. 这块板有什么

数据来自 `docs/ATK-DNRP2350AM_V1.0.PDF`（原理图两页）并与官方 wiki 交叉核对。

| 设备 | 类 | 引脚 | 说明 |
|---|---|---|---|
| `led0` | LED | GPIO3 | 蓝色 LED，**低电平点亮**（阳极经 510R 到 3V3） |
| `key0` | KEY | GPIO2 | KEY1 / BOOT 键，经 1K 到地，**0 = 按下**；复位时按住进 ROM BOOTSEL |
| `lcd0` | DISPLAY | DC=8, CS=9 | ST7789V2，135×240，4 线 SPI1：SCK=10 MOSI=11，背光=25 |
| `tf0` | STORAGE | 10 | TF 卡座走 SDIO：CLK=10 CMD=11 D0..D3=12..15（**与 lcd0 共用引脚**） |
| `uart0` | UART | 0 | CH343P 板载 USB 串口，TX=0 RX=1，控制台 115200 |
| `usb0` | USB | — | 原生 USB 1.1 设备口，BOOTSEL + CDC 控制台 |
| `adc0..4` | ADC | 26..29 | 12 位，3.3 V 参考；`adc4` 是片内温度传感器 |
| `flash0` | FLASH | — | W25Q32 **4 MiB** QSPI（32 Mbit），内核 + littlefs |

两个硬件事实值得单独记住：

* **背光是低电平点亮。** Q2 是 S8550（PNP），发射极接 3V3、基极经 1K 接 GPIO25
  并有 10K 上拉、集电极经 10R 供 LEDA。所以 GPIO25=0 才亮，名字正好相反。
  这个看错一次，屏幕就是全黑，而且和 SPI 没配好长得一模一样。
* **LCD 和 TF 卡共用 GPIO10/11，但可以互斥使用。** 屏用它们做 SCK/MOSI，
  卡座做 CLK/CMD。TF 卡在 **SPI 模式**下 CMD=MOSI、DAT0=MISO、DAT3=CS，
  正好落在同一组线上：LCD 的 CS 是 GPIO9，卡的 CS 是 GPIO15(DAT3)，
  两个片选各管各的，只要内核保证任一时刻只有一个为低即可。表里给 `tf0`
  打了   `TOS_DEV_SHARED`，`/proc/dev` 会标出来；`plat_display_sync()` 就是
  留给未来 TF 驱动的总线移交闸门（先把 LCD 的 DMA 尾巴收干净再切 CS）。
* **QSPI flash 是 4 MiB，不是 2 MiB。** 原理图元件注释写 W25Q32（32 Mbit =
  4 MiB），但库参考沿用 W25X16（2 MiB），早期文档跟着写错。上板实测：
  XIP 读 `+0` 与 `+4 MiB` 得到同一组向量表字节，`+2 MiB` 是未编程的 0xFF，
  即地址在 4 MiB 处回绕。内核占前 512 KiB，littlefs 从 0x80000 起覆盖剩余
  **3.5 MiB**（896 块）；刷件里的 FS 镜像仍只有 300 KiB，挂载时 `lfs_fs_grow`
  撑大，旧数据块由 littlefs 按需擦除回收。

## 3. 屏幕：一块能当终端用的屏

屏不是「又一个外设」，是**一块屏幕**：用户态往里写字，它就像终端一样换行、滚动。

* 分辨率 135×240，字符 6×8（5×7 字形 + 1 像素间距），文本区 **40 列 × 16 行**。
  实测正确：`screen info` 返回 `240x135 pixels, 40x16 cells`，蓝底白字满屏正确。
* `MADCTL = 0x70`（横屏），控制器 RAM 是 240×320 而玻璃是 135×240，所以每个窗口
  都要偏移 `(40, 53)`。这是这块屏的标定值，写错就是画面整体偏一格。
* SPI1 在 `clk_peri = 150 MHz` 下取 4 分频 = **37.5 MHz**（ST7789 写上限约
  62 MHz，板上走线很短）。像素走 **DMA 通道 10**：整行 3840 字节一个窗口、
  一次 DMA，CPU 只负责填缓冲；下一次事务才懒同步等尾巴。整屏滚动 16 行
  约 13 ms 总线时间，不再是旧版的 86 ms 轮询。
  **坑**：DMA 块出厂处于保持复位（`RESET_DMA`=bit2），必须先解复位；
  PL022 还必须写 `SSPDMACR |= TXDMAE|RXDMAE` 放开 FIFO 的 DREQ，
  否则 DMA 启动后永远 BUSY。两者缺一都会让屏/整机看起来像挂死。
* 字形表由 `scripts/gen-font5x7.py` 生成（`board/rp2350/font5x7.h`），
  生成器同时输出一张 40×16 的预览图，改字形先看预览再上板。

### 内核侧

`kernel/screen.c` 是通用文本屏：光标、换行、退格、滚动、颜色、反显。
板子只需实现六个钩子（`hwdev.h`）：

```c
int plat_display_info(int idx, int *w, int *h, int *cw, int *ch); /* 像素 + 字符格 */
int plat_display_cell(int idx, int col, int row, char c, uint16_t fg, uint16_t bg);
int plat_display_row(int idx, int row, const char *chars, int ncols,
                     uint16_t fg, uint16_t bg);  /* 整行快路径，可返回 -1 走逐格 */
void plat_display_sync(int idx);        /* 等 DMA 尾巴，给共总线的设备用 */
int plat_display_clear(int idx, uint16_t color);
int plat_display_backlight(int idx, int on);
```

注意 `plat_display_info` 返回的是**像素**，不是行列数。这一点写反过一次：
驱动报「40 列」，屏幕层又除了一次字符宽，结果终端缩成 6×2 挤在左上角。

文本屏只有 640 字节的影子缓冲，滚动靠重绘——因为这块屏的读线没接出来，
没法和 ST7789 的 RAM 对读。现在整行重绘走 DMA（一行 3840 字节，约 0.8 ms
总线时间），比逐格发 40 次窗口命令便宜得多；代价是驱动里要留一个 3.8 KiB
的静态行缓冲，内核堆为此从 64 KiB 调到 60 KiB（exec 最大需求约 49 KiB，
余量仍够）。

### 控制台镜像

`kernel/console.c` 的 `con_putc()` 是内核里字符的唯一出口：UART 收到什么，
屏上也写什么，包括按键回显。默认开启，`screen mirror off` 可以关掉。
代价是每个字符多约 150 µs，`cat` 大文件会明显变慢——这是「把屏当终端」的
真实成本，不是 bug。

### 用户态

```sh
screen                 # 状态卡：型号、分辨率、开机时长、用法
screen dev             # 设备表（屏上紧凑版，内核画，不折行）
screen text hello      # 在光标处写字
screen clear red       # 铺满颜色（black/white/red/green/blue/cyan/yellow/0xRRGGBB）
screen cursor 0 3      # 定位
screen mirror on|off   # 控制台是否镜像到屏
screen invert on|off   # 反显
screen backlight on|off
screen info           # 几何 + 原始 ioctl 值
```

也可以完全走 shell 重定向，不需要这个程序：

```sh
echo hello > /dev/lcd0
cat /proc/dev > /dev/lcd0
```

### 像素视图：`/dev/fb0`

同一块屏还有第二个节点 `/dev/fb0`，**不经过字体、不经过字符网格**，
直接写 RGB565 像素（大端，高字节先，与 ST7789 的 SPI 顺序一致）：

```c
int fd = open("/dev/fb0");
ioctl(fd, FB_OP_INFO, 0);                  /* -> (width<<16)|height */
ioctl(fd, FB_OP_RECT, FB_RECT(x, y, w, h));/* 选矩形，x/y/w/h 各 8 位 */
write(fd, row, w * 2);                     /* 一行像素，可连写 h 行 */
ioctl(fd, FB_OP_SYNC, 0);                 /* 等 DMA 尾巴收干净 */
```

`FB_OP_CLEAR` 填整屏。驱动把用户缓冲逐行拷进内核行缓冲再交给 DMA，
所以 `write()` 一返回调用方就能复用自己的缓冲；每行拷贝前都会先同步
上一行，避免刚修过的"上一行 DMA 读到被覆盖的字节"脏屏问题。
板级钩子是 `plat_display_scan()`，没有像素能力的板子弱默认返回 -1。

配套命令：

```sh
fb                          # 渐变 + 棋盘 + 实心圆的演示位图
fb info                      # 240x135, 16 bpp
fb bars                      # 八色条
fb clear 0x000040            # 整屏填色（也支持 black/red/cyan 等名字）
fb rect 20 20 60 40 0xFF0000 # 填一个矩形
lcdconsole off               # 关掉屏上的 console 镜像，画面才不会被提示符冲掉
lcdconsole on                # 恢复（等价于 screen mirror on|off）
```

实测整屏 240x135 位图从命令发出到返回约 0.5 s（含 ELF 加载）；
`lcdconsole`/`fb` 都不参与启动，不影响开机速度。

## 4. 其它设备的用法

```sh
cat /proc/dev              # 整张设备表
cat /dev/key0              # 1 = 正在按下
echo 1 > /dev/led0         # 点亮（内部做低电平取反）
cat /dev/led0              # 读回逻辑状态
echo on > /dev/led0        # on/off 和 1/0 都认
cat /dev/adc3              # ADC 原始值
adc 4                      # 片内温度传感器，打印原始值与 mV
```

**ADC 的 bring-up 是这块板自己的事**：`RESET_ADC` 上电是**断言**的（模块被按在
复位里），`CLK_ADC` 也是**关的**，谁都不碰就一直超时（早期的
`adc: conversion failed` 就是这个）。`board/rp2350/gpio.c` 的 `adc_init_hw()`
在第一次读时补上：解除复位 → 开 `CLK_ADC`（该芯片的 AUXSRC **没有 clk_sys**，
只有 pll_usb/pll_sys/rosc/xosc，所以走 PLL_SYS 四分频 = 37.5 MHz，低于 48 MHz
上限）→ 把 26..29 的 pad 拉出隔离、关掉数字输入。`adc4` 还要额外置 `CS.TS_EN`，
并且第一次转换要丢弃（传感器需要一次转换才稳定）。

LED 的心跳：内核每秒翻转一次 GPIO3，作为「机器活着」的串口无关信号。
**用户第一次写 `/dev/led0` 后心跳永久停止**，否则用户设的灯会被定时器抢回去。

### `reboot` 必须走 ROM，不能用 AIRCR

`plat_reboot()` 现在调的是 ROM 的 `reboot`（`ROM_FUNC_REBOOT = 0x4252`，flags
`0x100` = `BOOT_TYPE_NORMAL | NO_RETURN_ON_SUCCESS`），和 `rom_enter_bootsel()`
同一个入口，只是 flags 不同。之前写 `AIRCR.SYSRESETREQ`，实测**板子会挂**：

- 主机侧 COM17（CDC）仍然枚举、设备状态 OK，但 `open()` 一律
  `PermissionError 13`；COM14（UART0）发 `\r` 完全无回显，Ctrl-D 也进不去
  BOOTSEL —— 内核根本没跑起来。
- 根因是 SYSRESETREQ 只复位内核和部分外设，USB 控制器和时钟树保持原样，
  ROM 之后的启动路径拿到的不是一次「上电」。
- 现在 `reboot` 能完整重走一遍启动：UART0 上从 `[B] clk=` 到 `[mpu]` 全有，
  CDC 重开直接是 `tos>` 提示符。AIRCR 那条留作 ROM 入口找不到时的兜底。

## 5. 没有做的部分

* **TF 卡驱动**：SDIO 栈是独立工作量，而且这块板上它和屏抢引脚，
  同时用本来就不成立。设备表里已经如实登记，将来要写驱动只需认领一行。
* **按键中断 / 长按**：现在只有轮询读电平。按键接在 GPIO2，可以走 GPIO 中断，
  但内核目前没有「用户态等一个引脚」的阻塞原语，做了也只能忙等。
* **PWM / 蜂鸣器 / 音频**：这块板没有蜂鸣器；PWM 属于引脚复用，暂未抽象。

## 6. 新增一块板要做什么

1. 在 `board/<name>/` 里实现 `platform.h` 的板级钩子（照 `board/rp2350/` 抄）。
2. 写一张 `board_dev_t` 表 + `plat_board_name()`，实现对应的类钩子；
   没有的类不用写，`kernel/hwdev.c` 里有 weak 默认值。
3. `internal/build/build.go` 加一个 target（`Board:` 指向上面的目录）。
4. `/proc/dev` 立刻就能列出新板子的设备，`/dev/led0` 这类节点自动出现。
