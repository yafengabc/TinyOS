# RP2350 默认开 kthreads + usb_pump，四镜像验收（2026-11-26）

```text
commit:            c33d897
change:            六个 RP2350 board.conf 默认 features 加 kthreads + usb_pump（不再写临时
                   -config）；rp2350rv-generic 补 M33 在 09da1cd 的严格模式。
                   基线镜像装不下三个线程程序的问题由 7881678 用"基线排除
                   kthreadtest/threadbench/prodcons"解决，本卡不含那部分
baseline:          9db6fcd
target/config:     build all（18 目标，默认配置）+ rpi-pico2 四镜像默认构建
board/port/image:  Pico 2（RP2350）COM19 = CH343 UART0，BOOTSEL 期间不消失
toolchain:         go 1.27.3；clang 23.1.4；python 3.14.9
host:              Windows / 本机
T0:                gofmt 11 个文件未格式化（全在 .go，本轮没动）；go build/vet PASS；
                   go test 仅 TestPinnedToolchainSources FAIL（pre-existing：主工作区同失败）；
                   skip 0
T1:                18/18（x86 的 300 KiB 溢出已由 fc20ca6 修好，本轮重跑验证）
T2:                smoke rv32/rv32smp/arm 3/3 PASS；soak arm、rv32smp PASS，
                   rv32 FAIL（wheap 挂死，A/B 定位到远端 a004448，OPEN-ITEMS §24）
T3:                Pico 2 四镜像 accept33 19/19、21/21、20/20、22/22；
                   §5.4 关闭门槛所要求的两个 SMP 镜像四项全过（见下）
T4:                dd/rm 100 轮 ×2 PASS；reboot 20 轮 ×2 PASS（uptime 归零）
logs:              build/acc4-*.log、build/check-ktask.log、build/testlogs/*.log（本地，不入库）
notes:             exp/thread-sched；Pico 2 的 UART0 这次有输出（此前"UART0 哑"是接线
                   问题），所以验收走 UART0，--uf2 路径能抓到完整 banner
```

## 为什么改成默认

内核线程方案此前只在 `-config` 里逐镜像打开，板测要手写临时 conf，镜像漏写就悄悄
退回旧模型。用户决定以后默认就是这套，所以写进板级 `board.conf`，构建命令回到
`-target <board>`。

## §5.4 关闭门槛（Pico 2 两个 SMP 镜像）

| 项 | rpi-pico2-smp | rpi-pico2-rv32-smp |
|---|---|---|
| 心跳 6/6 | PASS | PASS |
| dd/rm 100 轮 | 100/100 | 100/100 |
| smpbench 1250000 | **2.00x**（37 / 35+37 ms） | **1.89x**（52 / 55+53 ms） |
| reboot ×20 | 20/20，prompt 0.45–0.46 s，uptime 0.09 s | 20/20，prompt 0.66 s，uptime 0.34 s |
| `/proc/ktask` | `0 4096 208 208 usbpump` | `0 4096 652 652 usbpump` |

单核两镜像的 accept33（19/19、20/20）和四镜像的 `/proc/ktask` 只作排查记录，
不作关闭依据（§5.4 写明那张表是 ATK 的 LCD/TF 假设）。

## 判据用 `/proc/ktask`，不用 banner 行

SMP 两轮的启动日志里没有 `[usb] pump thread pid N`（第二核与 console 抢锁，整块丢行），
UP 两轮有。严格模式的本意是"泵没起来控制台就哑"，但 `NOT started` 这行同样可能被启动期
丢掉，所以判据只能是运行期状态：`/proc/ktask` 有 `usbpump`，且 `uptime` 与本次刷机对得上。

## 平台事实（不是回归）

`kthreadtest` 在 RV32 上稳定 1 个 failure：`the holder died on its fault`。用例要求线程
在访存 fault 里死掉，但 RP2350 bootrom 把 PMP 8..10 硬编码，地址 0 在 U 态不 fault。
ARM 同镜像 `all ok (0 failure(s))`。

## 没做的

- **ATK 四个镜像**：没有板。同一份源码但引脚/设备表不同。
- **T2 soak 的 rv32**：被远端 `a004448` 的 `wheap` 挂死挡住（OPEN-ITEMS §24），
  不是本轮改动——只切 `kernel/syscall.c` 的 A/B 就能复现/消除。
- **负向验证**：让 `process_ktask_create` 必然失败、确认控制台真哑——泵一停 CDC 与
  UART0 都没有字符入口，只能物理按 BOOTSEL。
- **RP2040 / ESP32-C3 / QEMU / x86 的默认**：本卡提交时仍关闭。RP2040 的泵迁移
  （`fc70612`）在随后落地，C3 的内存账另说（用户决定回头再定）。
