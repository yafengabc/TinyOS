# 2026-11-20 RP2040 唯一 USB 序列号

## 问题

RP2350 两个端口从 `SYS_INFO_CHIP_INFO` 取 64 位芯片 ID 做 CDC 的 `iSerialNumber`；
RP2040 没有这个寄存器，一直写死 `"0001"`，两块板插同一台主机会被当成同一个设备。
`OPEN-ITEMS.md` 第 2 条要求从 flash 发 0x4B 读唯一 ID，本轮实现并上板验证。

## 实现

- 新弱钩子 `plat_flash_unique_id(uint8_t out[8])`（`kernel/platform.h`，
  `kernel/main.c` 默认返回 0）。
- `board/rpi-pico/platform.c`：因为调用点在 `plat_console_init()`（早于
  `plat_board_init()`），三个 ROM flash 辅助函数按需解析；RAM 里的
  `rp_flash_cmd(op, nframes, out)` 复制 boot2、`connect_internal_flash`、
  `flash_exit_xip`，用 QSPI pad override 拉 CS（SSI_SER 无效），逐帧收发，
  所有等待都有上限，最后 `flush_cache` + boot2 恢复 XIP。
- `drivers/usb/rp2040/usb_cdc.c`：新增 `g_serial[17]`，`usb_cdc_init()` 开头填 16 位
  十六进制；描述符的 `iSerialNumber` 从常量改为 `g_serial`。读不到时回退 16 个 0。

## 排查过程（值得留档）

第一版按"4Bh + 4 dummy + 08h 长度字节 + 8 字节 UID"解析，结果全 FF 被判失败并回退。
同一点做 RDID 对照能读到 `ef4015`，说明早期 flash 访问没问题。一次刷机跑四组原始帧：

```text
[uid] 9f   ff ef 40 15 00
[uid] 4b13 ff ff ff ff ff e6 60 9c b2 d3 4a b7 33
[uid] 4b17 ff ff ff ff ff e6 60 9c b2 d3 4a b7 33 e6 60 9c b2
[uid] 5a13 ff ff ff ff ff 53 46 44 50 05 01 00 ff
```

结论：这块 W25Q16JV 的 4Bh 应答**没有 08h 长度字节**，UID 直接在 `rx[5..12]`；
pico-sdk 的 `flash_get_unique_id()` 读的也是同一个窗口。假设长度字节存在，
等于把一个好读数扔掉。SFDP（5Ah）返回 `53 46 44 50` = "SFDP"，说明多字节帧也没问题。

日志：`build/rp2040-uid-sweep.log`、`build/rp2040-uid-final.log`。

## 上板结果

```text
[uid] e6 60 9c b2 d3 4a b7 33
CDC COM29 serial = E6609CB2D34AB733
```

- Windows 的 CDC 序列号字段从 `0001` 变成 `E6609CB2D34AB733`；
- `reboot` 后仍是同一个序列号（同一块 flash，符合预期）；
- `flash.py --smoke`（走 UART）**7/7 PASS**；启动日志无 trap。

## 双板验证（同日 18:40 补）

用户接上第二块 RP2040（**只有板载 CDC、没有 UART0**，所以看不到 `[uid]` 启动行），
按住 BOOTSEL 刷入同一镜像 `rpi-pico-smp`。Windows 同时枚举出两个 CDC，序列号不同：

```text
COM29  E6609CB2D34AB733    (第一块，flash 2 MiB detected=2 MiB)
COM30  464A383332112312    (第二块，/proc/flash: detected 16 MiB, configured 2 MiB)
```

第二块板上 `uptime`、`cat /proc/flash` 正常；同一进程内发 `reboot`，uptime
**29.01 → 3.05 s**（确认真复位），重启后两个序列号都不变。"两块板同插不撞号"
从"按部件保证"变成**实测通过**，且两块 flash 型号/容量不同也能各自取到 ID。

## 边界

- 第二块板只有 CDC、没有 UART0，`[uid]` 原始帧只在第一块（W25Q16JV）上核过；
  第二块 16 MiB flash 的 4Bh 应答窗口按同一实现取 `rx[5..12]`，结果是有效唯一 ID。
- 若 4Bh 不应答或读到全 0/FF，仍回退到 16 个 0（所有板相同）——但至少不会静默挂死；
  `[uid]` 启动行会打出实际读到的字节，便于发现回退。
- 本轮没有重跑 #31 的冷启动三组数据（那是在加这个早期 flash 命令之前测的）；
  早期只多一次有界 SSI 命令，smoke 与重启已验证功能正常。
