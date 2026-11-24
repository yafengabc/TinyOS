# ATK 四镜像回归 + ARM SMP 控制台死锁定位与修复

```text
commit: 8e92fa4（本轮修复；改动基于 6b15b8b，ATK 结果在修复后的工作区测得）
change: ATK 四镜像（ARM/RV32 × 单核/双核）板级回归；定位并修复
        atk-dnr2350am-smp 在 prot heap + deep 3 负载下的控制台死锁
baseline: none
target/config: atk-dnr2350am、atk-dnr2350am-smp、atk-dnr2350am-rv32、
               atk-dnr2350am-rv32-smp / defaults
board/port/image: ATK-DNR2350AM (RP2350) / UART COM14 (CH343)、USB CDC COM26
                  / build/tinyos-atk-*.uf2
toolchain: go 1.27.1 windows/amd64, clang 23.1.4, python 3.14.9
host: Windows（Asia/Shanghai）
T0: NOT RUN（本轮未跑 go test/go vet）
T1: PASS（4 个 ATK target 构建；修复后两个 ARM 镜像重新构建，各 2,097,664 B）
T2: NOT RUN（无 QEMU）
T3: PASS（ARM SMP accept33 21/21、ARM 单核 19/19、两镜像 heartbeat 6/6 +
             32 条 hammer 无 STALL；RV32 两镜像本轮未重测，见注 3）
T4: PASS（-smp 按 TESTING §5.4：dd 200/200、smpbench 2.06x、reboot 20/20；
          单核只做 32 条 hammer）
logs: 控制台输出，未另存
```

## 复现

`atk-dnr2350am-smp` 上跑 `accept33`，走到 `prot heap` ×16 + `deep 3` 之后控制台
冻结，1200 ms 后板级看门狗打印 `[STALL]` 并复位。STALL 记录：死核为 CPU0，
`mask=1 site=3`（`SPIN_TRY_GATE`）、`idle=1`、`usb=9`、`greg=00000000`，
`[PREV] m0s=3`（上次掩码区从 site 3 进入），不在故障处理器内。

`site=3` + `idle=1` 把范围收到一处：`sched_idle` 里**关中断**的 `tty_poll()`
走到了 `usb_cdc_getchar()`，而该函数当时会取 USB 锁并进入 TinyUSB
（`tud_cdc_read_char()`）。关中断路径没有 tick 可退，另一个核又可能在持锁跑整趟
状态机，于是停在那里等看门狗。RV32 两镜像同负载全过，说明这是 ARM USB CDC
控制台路径独有的问题，不是调度器或 SIO 门本身。

## 修复（`8e92fa4`，基于 `6b15b8b`）

1. `drivers/usb/rp2350/usb_cdc.c`：驱动自持 RX 环形队列（256 B）。
   `usb_task_run()` 在持锁、正常上下文里把 TinyUSB 已完成的字节搬进队列；
   `usb_cdc_getchar()` 改成纯出队——不取锁、不进 TinyUSB、不等待，可在掩码上下文
   安全调用。队满时余量留在 TinyUSB 里，下一次状态机跑再搬。
2. `arch/arm/platform.c`：`sio_gate_claim_bounded()` 从“超时即 panic”改为可恢复，
   首次超时打印 `[GATE] cpu N lost gate, forcing release`、释放门后重试一次，
   第二次超时才 `[GATE!]` + `plat_panic_halt()`。门只保护三指令窗口，活着的持有者
   不可能占这么久，等下去只会把一个死核变成两个。
3. `board/rp2350-generic/platform.c`：STALL 的 `greg=` 改读 `SPINLOCK_ST`
   (`0xD0000000 + 0x5C`)。原来读 `SPINLOCK[31]` 会顺手把内核门抢走且永不释放，
   诊断自身制造了假 `[GATE!]`。同时删掉临时插桩（`kernel/process.c` 的
   `g_sched_site`、`arch/arm/platform.c` 的 `GATE_MARK`/`g_gate_*` 计数、
   STALL 里的 `sched=`/`gt/gh` 字段），`kernel/diag.h` 只留
   `TOS_MASKSITE_FAULT = 6`。

## 结果

| 项 | atk-dnr2350am | atk-dnr2350am-smp |
|---|---|---|
| accept33 | 19/19 PASS（0.425 s） | 21/21 PASS（0.375 s） |
| heartbeat | 6/6 | 6/6 |
| smptest | — | 11/11 |
| smptest --func | — | 6/7（注 1） |
| dd_stress 200 轮 | — | 200/200 |
| smpbench 1250000 | — | 2.06x |
| reboot_soak 20 次 | — | 20/20 |
| hammer（16×`prot heap` + 16×`deep 3`） | 32 条无 STALL | 96 条无 STALL，`journal intact` 16/16 ×3 轮 |

修复前的同一负载在同镜像上 100% 复现 STALL/复位；修复后干净构建复测，三个会话
各 32 条命令全部通过。`accept33` 覆盖 `deep 3 journal intact`、`smpbench ≥1.8x`、
flash-then-boot 计时。板子最后恢复为 `atk-dnr2350am-smp` 干净镜像并跑 heartbeat 6/6。

## 注

1. `smptest --func` 6/7，唯一 FAIL 是 `py demo.py`：RP2350 默认镜像自 `f7f962c`
   起不带 `pvm`（opt-in），与 Pico 2 卡片一致。
2. 修复只动 USB CDC 读路径与 SIO 门超时策略，不改调度器；`f91db82..6b15b8b`
   也没有改 `kernel/process.c`、`arch/arm`、`kernel/cpu.c`，所以这不是本轮拉取
   引入的回归。
3. `atk-dnr2350am-rv32` / `-rv32-smp` 本轮没有重刷：它们的 grain 修复
   （`3dbcaa7`）此前已在真机上确认，且 RV32 不经过 USB CDC 掩码读路径。两张卡片的
   结论仍然有效，但不算本轮的实测行。
4. USB 锁持有窗口内的逐指令卡点没有单独证实；`usb=9` 也可能来自
   `usb_task_run()` 的取锁步骤。根因结论停在“掩码读路径会等锁/进 TinyUSB”这一层，
   修复按契约（掩码上下文只碰本核数组）做，不依赖更细的定位。
