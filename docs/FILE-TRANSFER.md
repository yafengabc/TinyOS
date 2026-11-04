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
                         <-  "READY <chunk>\n"
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

## `READY` 里为什么要带块大小（陈旧镜像的教训）

内核（`0x0`）和文件系统镜像（`0x80000`）是**分开烧**的，`flash.py --keep-fs`
只写内核。于是板子很容易跑着一个**旧构建的 `rx`**，而宿主用的是新协议。这个
组合没有任何显式错误，只有一串看不懂的 NAK：

- 旧 `rx`（`8310a29`）是 `CHUNK 512 / LINE_MAX 768`，新协议发的是 1024 字节块
  的 **1377 字符**行。
- `read_line()` 在 `len >= LINE_MAX - 1` 时返回 -1，于是**前 768 个字符被丢掉**；
  而 `expected_seq == 0` 时该分支**静默不回复**。
- 剩下的 609 字符（从 base64 中间开始）被当成下一条行 → `NAK 0 r1[H3bx...]`。

现场测量（`RAW_Q` 是 1 KiB 还是 2 KiB 毫无影响）：

```
[raw] in=1377 drop=0 take=1377 | uart bytes=1378 irq=38 ovf=0
```

**内核一个字节都没丢**：1378 字节进了 38 次接收中断，raw 环 `in=1377 /
drop=0`，全部交给读者，硬件 FIFO 溢出计数 `ovf=0`。丢字节的是那个旧 `rx`
自己的行缓冲——按"中断没收到/环不够大"去修只会白改一遍。

所以 `READY` 现在带上块大小（= 协议版本），两个宿主工具都在握手时就比对：

```
FAILED: board's rx does not report its chunk size (stale filesystem image): flash without --keep-fs
```

`NAK` 的 `r1` 也补了 `n=<行长>`，下次再看到"片段从中间开始"能立刻判断是
截断（`n` 正好等于某个上限）还是真丢字节。

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
| USJ 发送环形缓冲 1 KiB + `SERIAL_IN_EMPTY` 中断 | `board/esp32c3/usj_console.c` | 控制台**永不**阻塞在 USB 上；没有主机就填满即丢（和 UART 满发送器同性质） |
| USJ 接收中断（`SERIAL_OUT_RECV_PKT` = bit 2，INTMTX source 26 → CPU line 4） | 同上 + `board/esp32c3/platform.c` | 原生 USB 那路的输入也即时排空 |
| raw 环 256 B → 512 B（当时定 1 KiB，后按实测缩回，见下），cooked 队列 → 1 KiB 字节池 | `kernel/tty.c` | 一次突发整段落进队列，不必按 tick 切片（两个队列的来历见下） |
| `process_console_kick()`，接收中断里直接交数据给阻塞的读者 | `kernel/process.c`、`arch/rv32/platform.c` | 读者不再等下一个 tick 才被唤醒（原来每 tick 一次 = 吞吐天花板） |
| 单次交接上限 128 → 1024 B | `kernel/process.c` | 配合大环，批量读的程序一次拿到更多 |

现在的参数：

- **块 1024 B，D 行 ~1366 字符**，一次发完（`writePaced` 按 256 字符一段、
  1 ms 间隔，纯属保险；USB-UART 桥自己会按 115200 节流）。
- **raw 环 512 B，实测容量 511 B（44 ms 线速）**：接收中断把字节直接交给阻塞
  的读者，环只在读者不在 `read()` 上时积压。`QUANTUM` 改成 1 ms 之后，被抢占
  一次的代价只有 12 B，真正要覆盖的是读者**正在跑、不读**的那段时间。
  原来按"5 tick = 50 ms、115200 走 576 B"定的 1 KiB 因此超配了一个数量级。
  缩之前先量（`user/rawsink.c` + `scripts/esp-c3/rawstorm.py`）：读者睡过一整
  个 4 KiB 突发再排空，能捞回来多少就是多少——`RAW_Q 512` 捞回 511、
  `1024` 捞回 1023，差值恰好 512。**128 B 硬件 FIFO 一点都不额外算**，因为接收
  中断一直在把 FIFO 搬进环里；环满即丢，`head == tail` 的空/满判据再吃掉 1 字节。
  512 够用的理由是协议本身：`rx` 先把块 `write()` 进 flash、**然后**才回 ACK，
  而主机在等 ACK，正好是写 flash 期间线上没有新数据——唯一无界的那段停顿被
  stop-and-wait 挡住了。64 KiB 传输在 512 下 3/3 verified，**7.7 KiB/s 与
  1024 时完全一致**。**cooked 行队列**是另一回事——它管粘贴时的
  预输入深度，现在是 1 KiB 的字节池（每条 2 字节长度 + 内容），实测粘 100 行
  短命令全部执行；
  `\r\n` 也只算一个换行，否则每行会多塞一个空行、把命令挤出队列。
- **粘贴的行数不再受"接收 FIFO 溢出"限制**：内核在 trap 里关着中断读 flash，
  128 B 的 FIFO 只存 11 ms 的输入，而 C3 的 UART 溢出后会装聋。现在溢出会
  复位 FIFO，块设备每次读写前先排空控制台，盲窗从"整个 exec"降到"一个扇区"。
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
