# 2026-11-20 ATK 板复现 `mcause=0x3`：等待恢复，#29 重开

## 背景

同日稍早，基于 Pico 2 的证据（28 次完整冷启动、20 次暖重启，0 trap）删掉了
RV32 的 `usb_cdc_wait()` 并关闭了 #29，关闭说明里明确写了「ATK 板未验」。
用户随后把 ATK-DNRP2350AM 接上台，本次验证补上了这个缺口。

## 复现

- 板子：ATK-DNRP2350AM（UART0 = `COM14` CH343，板载 CDC = `COM26`）；
  两块板通过各自唯一的 USB 序列号区分（`689c07f`）。
- 镜像：`TOS_KEEP_SYMS=1 go run ./cmd/tos build -target atk-dnr2350am-rv32-smp`，
  即删掉等待后的代码。
- 刷机：`bootsel_poke.py COM26 20 --dtr` → `G:\`；刷后首次启动。

现象与 11-19 完全一致：**CDC 节点从未出现，UART0 在 shell 提示符后 0 字节**；
但 `bootsel_poke.py COM14 20`（UART0，DTR 不置位）仍能踢回 BOOTSEL——tick 活着，
只有 shell 死了。日志：`build/atk-nowait-first-boot.log`。

带符号重刷并在刷机前打开 UART0 抓一次性日志，拿到现场：

```text
TinyOS shell (pid 1).
tos>
*** RV32 TRAP ***
breakpoint: mcause=0x3 mepc=0x10016362 mtval=0x0
ra=0x10015820 sp=0x200169f0 s0=0x00000000 s1=0x20018428 a0=0x00000000 a1=0x80000000
```

（刷机后的下一轮又出现同一现场，两次完全一致。）

## 符号定位

```text
$ riscv32-unknown-elf-addr2line -f -C -e build/kernel-atk-dnr2350am-rv32-smp.elf \
    0x10016362 0x10015820
usbd_control_xfer_cb     // mepc
tud_task_ext             // ra

$ riscv32-unknown-elf-objdump -d --start-address=0x10016320 --stop-address=0x10016380 ...
10016360: 4501  li a0,0
10016362: 9002  ebreak
```

对应源码 `third_party/tinyusb/src/device/usbd_control.c:157`：

```c
if (tu_edpt_dir(ep_addr) != _ctrl_xfer.request.bmRequestType_bit.direction) {
    TU_ASSERT(0 == xferred_bytes);      // <-- ebreak
```

`a1=0x80000000` 正是状态阶段报上来的非 0 长度：设备控制传输状态机与主机不同步。
这与 11-19 的推断一致，但这次是在**当前代码**上、有完整现场和符号反查。

## 恢复与判定

用 `bootsel_poke.py COM14 20` 回 BOOTSEL，重刷**恢复等待**的镜像
（`go run ./cmd/tos build -target atk-dnr2350am-rv32-smp`），
`pump_repro.py --uart-port COM14 --drive G:\` 抓启动：

```text
[U] usb hw=1 cfg=1 stage=0
TinyOS shell (pid 1).
tos> uptime
uptime: 2.193 s
CDC node: COM26 at +0.50 s
shell: ANSWERED
```

日志：`build/atk-wait-restored.log`。

**判定**：Pico 2 的证据不可移植。`usb_cdc_wait()` 已恢复（保留 `3/260` 的界，
没有加长），#29 重开并以「无法定位跨板差异」结案：留等待，把风险写进
`docs/OPEN-ITEMS.md` 第 4 条。不再因为一块板过了 20 次冷启动就删它；
以后要删必须在**两块 RV32 板**上都跑过无 trap 的冷启动。

## 未回答的

为什么同一份无等待镜像在 Pico 2 上 28 次冷启动无 trap，在 ATK 上第一次就 trap：
没有继续做 A/B。候选是主机枚举时序（USB 口/hub）、板级 USB 走线、flash 容量与
LCD/SPI 共用，但本次没有数据支持任何一个。要定位需要在两块板上控制主机侧条件
做同固件对照，别再靠单板证据下结论。

## 遗留

Pico 2 当时未接台，板上仍刷着无等待镜像；重新接上后要重刷带等待的同名 target。
