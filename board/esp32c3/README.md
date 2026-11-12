# ESP32-C3（`-target esp32c3`）

第一块真机目标：RV32IMC、单核、400 KiB SRAM、4 MiB SPI flash，代码 XIP 在 flash
里执行，文件系统也在 flash 上。板子通过 CH343 串口和板载 USB-Serial/JTAG 两条线
都能进 shell。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | RV32IMC，单核，M 模式起步 → U-mode 跑任务 |
| SRAM | 可用 `0x40380000..0x403DC710`（378 KiB），指令别名窗口 |
| Flash | 4 MiB；内核 XIP 从 `0x10000` 起，`0x80000` 起是 littlefs |
| 文件系统 | 3.5 MiB 分区；随镜像发布的只有 300 KiB，首挂载 `lfs_fs_grow` 扩到全分区 |
| 控制台 | UART0（CH343，GPIO20/21，115200）**和** 硬件 USB-Serial/JTAG CDC，同时输出、两路都能输入 |
| 定时器 | TIMG0 → INTMTX；启动时关掉 ROM 留下的 TG0 看门狗 |
| 任务窗口 | 16 KiB 粒度，默认栈 8 KiB、栈下护栏 4 KiB |
| 内核堆 | 80 KiB（`heap=` 可覆盖） |
| 进程表 | 8 槽（`max_procs=` 可覆盖） |

## 构建与烧写

```bash
go run ./cmd/tos build -target esp32c3
# -> build/kernel-esp32c3.elf
# -> build/esp32c3.bin          (烧到 flash 0x0)
# -> build/ramdisk-esp32c3.bin  (烧到 flash 0x80000)
```

用脚本一条龙（自动进下载模式、烧完自己复位并抓 banner）：

```bash
python scripts/esp-c3/flash.py                 # 假定已经 build
python scripts/esp-c3/flash.py --build         # 先 build 再烧
python scripts/esp-c3/flash.py --keep-fs -p COM4   # 只更新内核，保留文件系统
```

手工用 esptool 时就是 build 输出里提示的两条：

```bash
esptool.py write-flash 0x0     build/esp32c3.bin
esptool.py write-flash 0x80000 build/ramdisk-esp32c3.bin
```

CH343 的 DTR/RTS 接了自动下载电路，不需要手按 BOOT。`-target esp32c3` 没有 QEMU
模型：功能回归走 `rv32emu-c`（`tostest emu` / `tostest emutest`），它模拟同一套
RV32IMC。

## 配置

```bash
go run ./cmd/tos build -config configs/esp32c3.conf            # 内核 + shell，不带 WiFi
go run ./cmd/tos build -config configs/esp32c3-wifi.conf       # WiFi + DHCP/ICMP + 全部命令
go run ./cmd/tos build -config configs/esp32c3-wifi-nonet.conf # 只关联 AP
go run ./cmd/tos build -config configs/esp32c3-wifi-softmin.conf # 饱和式软浮点，再小 ~580 B
```

`[user] exclude` 里的程序需要 `/dev/wifi` 或 `/dev/net`，非 WiFi 配置会把它们排除。

## 移植进度

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| XIP 内核、MMU 64 KiB 映射、`.data/.bss` | ✅ 实机 | kernel-xip.ld，`XipFlashBase = 0x10000` |
| 双控制台（UART0 + USB-Serial/JTAG） | ✅ 实机 | 输出镜像到两路，输入取先到的 |
| littlefs on SPI flash（ROM 读 + SPI1 擦写） | ✅ 实机 | `dd` 写入/读回校验通过 |
| PMP 分段、U-mode、`ecall` | ✅ | 与 QEMU rv32 同一套 |
| WiFi（PHY/WPA2/小型 IPv4）+ `ping`/`http` | ✅ 实机 | `drivers/wifi/esp32c3/`，预编译 blob |
| `py` 解释器、管道、线程、邮箱、内核裁剪 | ✅ 实机 | 百行脚本可跑 |
| 软浮点 / Q16.16 | ✅ | 链接期按需保留 |
| SMP | — | C3 是单核 |

已实测过的数字（见 `docs/OPEN-ITEMS.md`）：内核裁剪全关 text −18.7 KiB、bss −4 KiB；
`proc_t` 856 B；邮箱开启 text +468 B、bss +1,088 B。

## 已知限制

- 用户区约 370 KiB，16 KiB 一个窗口：`sh`/`edit` 各 32 KiB，`py` 要 128 KiB。
- 内核与 WiFi 代码必须在 `0x10000..0x80000`（448 KiB）之内。
- WiFi blob 只针对 C3；`-target rv32` 不带。

## 相关文件

- `board/esp32c3/platform.c`：UART0/TIMG0/INTMTX/看门狗、SPI flash、MMU
- `board/esp32c3/kernel-xip.ld`、`kernel.ld`、`layout.c`
- `drivers/serial/esp32c3/`：USB-Serial/JTAG
- `drivers/wifi/esp32c3/`：WiFi/PHY/WPA2/IPv4 与 blob
- `scripts/esp-c3/`：真机烧写、验收与寄存器探针
