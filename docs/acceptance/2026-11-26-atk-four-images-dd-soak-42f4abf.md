# ATK-DNRP2350AM 四镜像 dd/rm + 30 分钟压测（2026-11-26）

```text
commit:            42f4abf（镜像构建时为 6a46477；42f4abf 只改语料测试脚本，不改固件）
change:            ATK 板四镜像 T4：逐个 dd/rm 100 轮 + soak_board 30 分钟墙钟
baseline:          fb680a1（Pico 2 四镜像 T4）
target/config:     atk-dnr2350am、atk-dnr2350am-smp、atk-dnr2350am-rv32、
                   atk-dnr2350am-rv32-smp，全部默认配置
board/port/image:  ATK-DNRP2350AM；UART0 CH343 COM14；四个镜像逐个刷入
toolchain:         go 1.27.3；clang 23.1.4；python 3.14.9
host:              Windows / 本机
T0/T1/T2:          NOT RUN（本轮只做板上 T4；镜像由 6a46477 构建）
T3:                NOT RUN（accept33 属 #70，另记）
T4:                PASS ×4（见下）
logs:              build/testlogs/atk-dd-soak4.log、build/soak-atk-<image>.log、
                   build/soak-atk-<image>.console、build/soak-atk-rv32smp-rerun.log
                   （本地，不入库）
```

## 结果

| 镜像 | `dd_stress.py --rounds 100` | `soak_board.py -m 30` | cycles |
|---|---|---|---|
| `atk-dnr2350am` | DD OK 100/100 | PASS | 173 |
| `atk-dnr2350am-smp` | DD OK 100/100 | PASS | 165 |
| `atk-dnr2350am-rv32` | DD OK 100/100 | PASS | 218 |
| `atk-dnr2350am-rv32-smp` | DD OK 100/100 | PASS | 216 |

命令原文（每个镜像重复）：

```sh
python scripts/rp2350/bootsel_poke.py COM14 25
python -c "import sys; sys.path.insert(0,'scripts/rp2350'); import flash as fw; r,_=fw.bootsel_drive(); fw.flash('build/tinyos-<image>.uf2', r)"
python scripts/rp2350/dd_stress.py -p COM14 --target <image> --rounds 100
python scripts/rp2350/soak_board.py -p COM14 -m 30 --image <image> -l build/soak-<image>.log
```

`free` 前后 kernel heap/tasks 无漂移（除峰值水位），每轮 `deep 3` 有 `stack grown`
与 `journal intact`，`piperace` 全 ok，无 `KERNEL PANIC`。

## 中断与重跑

`atk-dnr2350am-rv32-smp` 第一次跑到 152 cycle / 1267.8 s 时机器重启，进程被杀，
判定行未落盘，**不算通过**。旧 console 日志文件被残留句柄锁住（`Permission denied`），
换用 `build/soak-atk-rv32smp-rerun.{log,console}` 重新刷机并跑满 30 分钟，
得到 `1802.4s ... PASS (30 min, 216 cycles, 10 commands)`。上表用的是重跑结果。

ATK 不是 RP2350 的关闭门槛（AGENTS.md 2026-11-19），这一行只作为同芯片不同板的
补充证据，不能顶替 Pico 2 的四个镜像。
