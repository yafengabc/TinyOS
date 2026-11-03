# 通过控制台传文件：tosput + rx

一台只有一根 USB 线的板子怎么把文件放进 littlefs？答案是走控制台：宿主的
GUI 工具（`tools/tosput`）和板上的接收程序（`user/rx.c`，镜像里叫 `bin/rx`）
说同一种行协议。零内核改动——raw 模式（`ttyraw()`，`edit` 已经在用）就是
全部的内核依赖。

## 用法

```
tosput.exe            GUI：选串口、选文件、Send
tosput-cli COM13 a.bin /a.bin     同一协议的无界面版（测试/脚本用）
```

板上等价物是 `rx /path`；GUI 自己发这条命令，不需要人在串口终端敲。

## 协议

ASCII 行帧 + 停等流控，base64（raw，无填充）编码：

```
宿主                              板（rx /path）
 "rx /path\n"            ->  shell exec rx；ttyraw(1)；create（TRUNC）
                         <-  "READY\n"
 "D <seq> <len> <b64>\n" ->  解码、写文件
                         <-  "ACK <seq>\n"   /  "NAK <seq> r<原因>\n"
 ...                     （seq 重复 = ACK 丢了，重答不重写）
 "E <crc32hex>\n"        ->  校验
                         <-  "DONE <crc32hex> <total>\n"
```

- **停等是协议的必要部分，不是保守选择**：内核在 trap 里跑、**中断是关的**
  （rv32 的 `mret` 才从 MPIE 恢复），`rx` 在 `write()` 里编程 flash 的那几毫秒
  板子完全听不见；接收中断只在用户态执行期间才有机会跑。宿主必须闭嘴等
  ACK——这是没有硬件流控的 UART 唯一可用的流控。
- **CRC32（IEEE）在板端对写进 flash 的字节计算**，所以它校验的是落盘内容
  而不只是线路。宿主的 `hash/crc32.ChecksumIEEE` 与之同多项式。
- ^C（0x03）在任何时刻杀掉 rx——tty 在读任务死亡时自动恢复行律；半截文件
  留在盘上，但结束时的 CRC 让"传坏了"是可检出的，而不是被默认。

## 尺寸的来历：曾经的瓶颈在内核的**输出**路径

第一版把吞吐钉在 1.0 KiB/s，归因是"内核 100 Hz tick 轮询收 FIFO"。这个判断
**是错的**，两轮真机排查后（见下）才找到真凶：

- UART0 的接收从 commit `94b8c6a` 起就**已经是中断驱动**的（RXFIFO_FULL 30 B
  + RXFIFO_TOUT 10 bit 时间），COM13 上的 CH343 走的正是这一路。
- 真凶是 **`usj_putc()`**：原生 USB-Serial/JTAG 那一路在没有 USB 主机时，
  `SERIAL_IN_EP_DATA_FREE` 永远是 0，于是每个字节都空转 40000 次超时才放弃。
  内核打印是在 trap 里、**关中断**执行的，于是每个字符都让 UART0 的接收
  FIFO 哑火几十微秒；一行 692 字符传进来，尾部（含 `\n`）正好掉在聋期里——
  症状是"宿主收不到 ACK，下一块与前一块粘连，NAK r1 里的片段从第 128 个
  字符开始"。把 USJ 输出临时改成直接丢弃，10/10 块立刻全通，实测钉死了因果。

修完之后：

| 改动 | 位置 | 作用 |
|---|---|---|
| USJ 发送环形缓冲 4 KiB + `SERIAL_IN_EMPTY` 中断 | `board/esp32c3/usj_console.c` | 控制台**永不**阻塞在 USB 上；没有主机就填满即丢（和 UART 满发送器同性质） |
| USJ 接收中断（`SERIAL_OUT_RECV_PKT` = bit 2，INTMTX source 26 → CPU line 4） | 同上 + `board/esp32c3/platform.c` | 原生 USB 那路的输入也即时排空 |
| raw 环 256 B → 1 KiB，cooked 队列 8 → 16 行 | `kernel/tty.c` | 一次突发整段落进队列，不必按 tick 切片（1 KiB 的来历见下） |
| `process_console_kick()`，接收中断里直接交数据给阻塞的读者 | `kernel/process.c`、`arch/rv32/platform.c` | 读者不再等下一个 tick 才被唤醒（原来每 tick 一次 = 吞吐天花板） |
| 单次交接上限 128 → 1024 B | `kernel/process.c` | 配合大环，批量读的程序一次拿到更多 |

现在的参数：

- **块 1024 B，D 行 ~1366 字符**，一次发完（`writePaced` 按 256 字符一段、
  1 ms 间隔，纯属保险；USB-UART 桥自己会按 115200 节流）。
- **raw 环 1 KiB 的来历**：接收中断把字节直接交给阻塞的读者，环只在读者等待
  被调度时积压，最坏一个 `QUANTUM`（5 tick = 50 ms）；115200 8N1 下 50 ms
  走 576 B，所以 1 KiB 有约 1.8 倍余量。装 4 KiB 是 3.5 倍余量，只是白占
  `.bss`（2026-11-03 收回，省 3 KiB）。
- 吞吐 **7.0 KiB/s**（64 KiB 实测 ~9.4 s，CRC 通过；原 1.0 KiB/s，**7 倍**）。
  理论上限：115200 → 11.52 KB/s，base64 4/3 膨胀后有效载荷 ≈ 8.6 KiB/s，
  所以已经跑到 **81%**。再往上只能改协议（窗口化，不等 ACK 连发）或换口。
- 块尺寸是**发送端的选择**（`len` 字段自描述），接收端只受 `CHUNK`（1024）/
  `LINE_MAX`（1536）限制；再大要同步改 `user/rx.c` 并重烧镜像。

**未验证**：本机没有插原生 USB 口（只插了 CH343），所以 USJ 的两条中断路径
（RX 中断、TX 环 + IN_EMPTY 中断）只做了代码级的对称实现，没在真机上跑到
主机侧。插上原生 USB 后应当：`tosput-cli` 用新出现的 COM 口传一遍，并确认
控制台输出完整（`cat` 一个大文件不该缺字）。已记在 `OPEN-ITEMS`。

## 调过的坑（都是实测出来的）

1. **Go 的 StdEncoding 带 `=` 填充**，rx 的解码器只收 raw——随机数据 512 B
   是 683 字符（尾组 3 字符），换成 RawStdEncoding 即可。
2. **解码器的尺寸上界**曾把"全组×3 + 尾组也按 3"算成 513 > 512 拒掉合法
   的 683 字符行；尾组是 2 字节不是 3。
3. **提示符 `tos> ` 后面没有换行**，按行等永远等不到；要按字节找。
4. **打开串口的 DTR/RTS 脉冲会复位 C3**（auto-download 电路），开机自检
   含擦块要 2–5 s——open 后先 settle 1 s，提示符等待给足 8 s。
5. 停等 + `ACK <seq>` 去重：ACK 丢失时宿主重发，rx 按重复 seq 重答不重写。
