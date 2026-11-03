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

- **停等是协议的必要部分，不是保守选择**：rx 在 `write()` 里编程 flash 的
  时候板子不可能收字节，宿主必须闭嘴等 ACK——这是没有硬件流控的 UART
  唯一可用的流控。
- **CRC32（IEEE）在板端对写进 flash 的字节计算**，所以它校验的是落盘内容
  而不只是线路。宿主的 `hash/crc32.ChecksumIEEE` 与之同多项式。
- ^C（0x03）在任何时刻杀掉 rx——tty 在读任务死亡时自动恢复行律；半截文件
  留在盘上，但结束时的 CRC 让"传坏了"是可检出的，而不是被默认。

## 尺寸的来历：板上 RX 一次只能吃 ~128 字节

内核 100 Hz tick 轮询收 FIFO（UART0 128 B / USB-SJ 64 B 包），两次 tick 之间
到达的突发超过 ~128 字节就丢（实测：cooked 命令 100 字符全过、150 起截断；
raw 单行 351 字符过、464 起丢——丢的往往包括行尾的 `\n`，症状是"下一行才
收到 NAK"或干脆沉默）。所以：

- **块 512 B，D 行 692 字符，宿主按 96 字符一段、30 ms 间隔发送**——每段都
  小于 FIFO，间隔大于两个 tick，保证排空。
- 吞吐 ≈ **1.0 KiB/s**（64 KiB 实测 64 s、CRC 校验通过）。慢，但对配置、
  脚本、小型固件镜像是够的；瓶颈在内核的轮询节奏，不在协议。
- 块尺寸是**发送端的选择**（`len` 字段自描述），接收端只受 `CHUNK`/`LINE_MAX`
  限制；将来内核若改掉这个限制（见 `OPEN-ITEMS`），把 sender 的
  `chunkSize` 调大即可，板端无需重烧——只要行仍装进 `LINE_MAX`（768）。

## 调过的坑（都是实测出来的）

1. **Go 的 StdEncoding 带 `=` 填充**，rx 的解码器只收 raw——随机数据 512 B
   是 683 字符（尾组 3 字符），换成 RawStdEncoding 即可。
2. **解码器的尺寸上界**曾把"全组×3 + 尾组也按 3"算成 513 > 512 拒掉合法
   的 683 字符行；尾组是 2 字节不是 3。
3. **提示符 `tos> ` 后面没有换行**，按行等永远等不到；要按字节找。
4. **打开串口的 DTR/RTS 脉冲会复位 C3**（auto-download 电路），开机自检
   含擦块要 2–5 s——open 后先 settle 1 s，提示符等待给足 8 s。
5. 停等 + `ACK <seq>` 去重：ACK 丢失时宿主重发，rx 按重复 seq 重答不重写。
