# USB RX 环满时丢一个字节（2026-11-24）

```text
TinyOS commit:     本提交（8e92fa4 的复盘补丁）
target/config:     atk-dnr2350am / -smp、rpi-pico2 / -smp、rpi-pico / -smp（构建）；
                   arm QEMU smoke；rv32/arm soak ×2；全 18 目标
board/port:        ATK-DNR2350AM（对方板上；本机 NOT RUN）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — arm smoke；soak 2/2
T3:                NOT RUN（本机无板；ATK 上的完整复测由板主做）
T4:                NOT RUN
```

## 背景

`8e92fa4` 把 ARM SMP 控制台死锁修掉了（`docs/acceptance/2026-11-24-atk-arm-usb-console-deadlock-6b15b8b.md`）。
复盘那三处改动时发现 RX 环的填充循环有一个顺序错误。

## 问题

```c
while ((c = tud_cdc_read_char()) >= 0) {
    uint16_t n = (usb_rxq_head + 1u) % USB_RX_Q;
    if (n == usb_rxq_tail)
        return;                 /* full: the rest stays in TinyUSB */
    ...
}
```

`tud_cdc_read_char()` 一旦返回就已经把字节从 TinyUSB 的 FIFO **取走**了；环满时
`return` 丢掉的是这个已经取出的字节，注释说的“留在 TinyUSB 里”并不成立。控制台输入
每个满环事件丢 1 字节，可能正好是换行。

## 修法

先看再取：

```c
while (tud_cdc_available() > 0) {
    uint16_t n = (usb_rxq_head + 1u) % USB_RX_Q;
    if (n == usb_rxq_tail)
        return;                 /* full: the rest really stays in TinyUSB */
    c = tud_cdc_read_char();
    if (c < 0)
        return;
    usb_rxq[usb_rxq_head] = (uint8_t)c;
    usb_rxq_head = n;
}
```

`tud_cdc_available()` 和读一样在 `usb_task_run()` 的锁内调用，所以这次多出来的一次
TinyUSB 调用仍然满足“填充在锁内、core 0”。

顺带把 STALL 里的 `greg=` 从裸地址 `0xD0000000u + 0x5Cu` 改成
`arm_memmap.h` 里新加的 `SIO_SPINLOCK_ST`（值相同，但名字和注释跟着寄存器走，避免下
一个读它的人误读 `SPINLOCK[n]`）。

## 验证

- T0 PASS；T1 18/18；T2 `arm` smoke PASS；soak rv32+arm 2/2 PASS。
- 本机没有 ARM 板，**T3 NOT RUN**：这个改动要在 ATK 上复测——建议一次粘贴 >256 字节
  的输入（让 RX 环填满一次），确认字节数与内容不少。
