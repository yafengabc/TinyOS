# usb_pump：失败不再回退到 PendSV/idle 钩子（2026-11-26）

```text
commit:            本提交
change:            plat_start_tasks() 在 TOS_USB_PUMP 下先交出唯一所有权再尝试
                   创建内核线程：g_idle_poll 在 process_ktask_create() 之前清空，
                   失败只打印 "[usb] pump thread NOT started" 并让控制台哑掉。
                   产品镜像可用 -DTOS_USB_PUMP_FALLBACK 显式保留旧行为。
                   本提交只含 M33（board/rp2350-generic）；Hazard3
                   （board/rp2350rv-generic）的同一改动留在工作区，等 RV32 上板
                   验证后单独提交。
baseline:          c580d20
target/config:     rpi-pico2-smp（ARM）、rpi-pico2-rv32-smp（Hazard3）；
                   板上用的是 build/k-pico2-smp-both.conf（kthreads + usb_pump，
                   临时组合，不入库）与 configs/rpi-pico2-usbpump.conf
board/port/image:  Pico 2（RV2350）COM27 原生 CDC
toolchain:         go 1.27.3；clang 23.1.4；Python 3.14.9
host:              Windows / 本机
T0:                FAIL（仅 TestPinnedToolchainSources：本 worktree 的
                   ../tinyos-script 与分支 pinned 拷贝不同步，pre-existing）
T1:                PARTIAL：构建 rpi-pico2-smp（本提交）与 rpi-pico2-rv32-smp
                   （工作区里对应的 Hazard3 改动），两者都成功；18 目标全量未跑
T2:                NOT RUN（本轮是板级改动，QEMU 不覆盖该路径）
T3:                PASS（Pico 2，严格模式，见下表）
T4:                NOT RUN
logs:              build/e3-strict-pump-board-20261126.log（严格模式）
                   build/e3-arm-pump-board-20261126.log（改前的 fallback 版，对照）
notes:             实验分支 exp/thread-sched；负向验证（制造创建失败看 CDC 是否真哑）
                   未做：泵停就没有 CDC 字符入口，只能物理 BOOTSEL 恢复
```

## 为什么改

改前 `plat_start_tasks()` 是「成功才清 `g_idle_poll`」：

```c
pid = process_ktask_create(...);
if (pid < 0) {
    kprintf("[usb] pump thread not started; keeping the polled hooks\n");
    return;                       /* 钩子还在，板子照常应答 */
}
g_idle_poll = NULL;
```

于是「内核线程没起来」和「内核线程起来了」在板上**完全同形**：CDC 都正常，
`/proc/ktask` 之外没有任何区别。板测用它判 pump 成功就是假通过——和
`docs/OPEN-ITEMS.md` §5c 记的那次「闸门没验 uptime 归零导致 15/15 假通过」是同一类错。

## 改后

```c
#ifndef TOS_USB_PUMP_FALLBACK
    g_idle_poll = NULL;           /* 先交出所有权 */
#endif
    pid = process_ktask_create("usbpump", usb_pump_task, NULL, 4096, 0, 0);
    if (pid < 0) {
        kprintf("[usb] pump thread NOT started (no slot or stack)\n");
        return;                   /* 没有泵 = 没人轮询 = CDC 全哑，测试立刻红 */
    }
```

`g_usb_ready` 为假时仍然直接 return：那时 USB 硬件本身没就绪，CDC 本来就没有
输出，不存在真假通过之分。

## 板上结果（严格模式，Pico 2 ARM SMP）

```text
ktask: slot   size   used     hw  name
  0    4096    248    248  usbpump
```

| 项 | 结果 |
|---|---|
| 泵线程 | `pid 1`，`size 4096`（按请求给），`used/hw 248` |
| `kthreadtest pipes` | prompt 回来，`[pid 7] killed` |
| `kthreadtest double-join` | `first 42, second -1` |
| `kthreadtest create-exit` | 200 cycles, `flagged 0` |
| 默认 `kthreadtest` | all ok (0 failure) |
| `smpbench` | 2.00x（single 148 ms，dual 142+148 ms） |
| `threadbench` | 57%（改前 61%，同一量级波动） |
| 长输出 `/bin/rcc` | 26,502 B 两轮逐字节一致 |
| 负载后 `/proc/ktask` | 仍 248/248 |
| `crash` ×5 | 5/5 prompt 回来，泵存活，uptime 不中断 |

严格模式与改前的 fallback 版结果一致，说明泵本来就在工作，不是回退路径撑着的。

## 没跑的

- **负向验证**：要做就得让 `process_ktask_create` 必然失败，然后确认 CDC 全哑。
  但泵一停 `bootsel_poke.py` 就进不去（它靠 CDC 收字符），Pico 2 的 UART0 又不通，
  只能物理按 BOOTSEL。用户决定跳过；这条闸门的有效性目前只有代码保证，没有实测。
- **RP2040 / ATK-DNRP2350AM 真机**：RP2040 的泵还在 tick 里（未迁移），ATK 上跑的是
  另一版镜像。
- **T1 全量 18 目标、T2 QEMU**：本轮只构建了改动的两个板级目标。
