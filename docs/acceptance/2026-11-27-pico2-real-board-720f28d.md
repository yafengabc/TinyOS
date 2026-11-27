# Pico 2 实物四镜像 T3（2026-11-27）

```text
commit:            720f28d（exp/thread-sched）
change:            无代码改动；在真实 Pico 2 上跑 #74–#77 的 T3
target/config:     rpi-pico2、rpi-pico2-smp、rpi-pico2-rv32、rpi-pico2-rv32-smp
                   全部默认配置（boot trace 开）
board/port/image:  Pico 2 实物（RP2350A，8 MiB W25Q64 克隆板），
                   UART0 GP0/GP1 → CH343 COM19
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
T0:                NOT RUN（本次只做板级 T3，见 0.5.0 卡）
T1:                NOT RUN（同上）
T2:                NOT RUN（同上）
T3:                PASS 四张 accept 20/20、22/22、21/21、23/23；
                   kthreadtest：ARM 两张 all ok，RV32 两张 1 failure（#72）
T4:                本次不重跑，见「T4 复用」一节
logs:              build/pico2real/（本地，不入库）
```

## 为什么这次是真板

上一轮 0.5.0 的 Pico 2 四镜像是在 ATK（4 MiB）上代跑的，用 `accept33 --df 3670016`
按 4 MB 判。这一轮是**真正的 Pico 2 板**：JEDEC 探测到 `0x00800000`（8 MiB），
`df` 是 7,864,320、`lfs_blocks` 1920，`accept33` 不需要任何 `--df` 覆盖——期望值
（`IMAGES` 表里的 7,864,320）就是板子的真值，判据没有放宽也没有代换。

## T3 结果

| 镜像 | accept33（`--dd-rounds 100`） | kthreadtest | memprot | 备注 |
|---|---|---|---|---|
| `rpi-pico2` | **20/20** | all ok (0) | on | ARM UP，无 T4 |
| `rpi-pico2-smp` | **22/22** | all ok (0) | on | smpbench **1.98x**；dd+rm 100 轮 PASS |
| `rpi-pico2-rv32` | **21/21** | **FAILED (1)** | on | 见下，属 #72 |
| `rpi-pico2-rv32-smp` | **23/23** | **FAILED (1)** | on | smpbench **1.94x**；dd+rm 100 轮 PASS |

四张的 `df total` 都是 7,864,320、`dd+rm 100 rounds` 全 PASS、`memprot: on`，
与 `/proc/cpuinfo` 一致。

### RV32 的那一条 failure

`kthreadtest: FAIL the holder died on its fault`，两张 RV32 各 1 条，ARM 两张没有。
`/proc/cpuinfo` 报 `memprot: on`，测试据此走严格分支要求退出码 139，但地址 0 的
store 并没有触发 fault，线程正常退出。这就是 **#72**（"no-MPU ports (RP2040) and
Pico 2 RV32"）——`plat_has_memprot()` 在这条路径上说了 on，而实际保护没有覆盖地址 0。
其余用例（含 `sibling switches reused the protection regions`、四条栈溢出）全部通过。

按 #76/#77 自己的判据（"`kthreadtest: all ok (0 failure(s))`"），这两条不能关闭；
#74/#75 的 T3 判据满足。

## T4 复用（本次不重跑）

按新判据，30 分钟压测可以用任一款 RP2350 的 SoC 做，ATK 上已有结果，不重复：

| 镜像 | T4 证据 | 出处 |
|---|---|---|
| `rpi-pico2-smp` | PASS，30 分钟 / 142 轮 | `2026-11-27-release-0.5.0-a5e4909.md`（ATK，ARM SMP） |
| `rpi-pico2-rv32-smp` | PASS，30 分钟 / 241 轮 | `2026-11-26-issue46-thread-matrix-pico2-2a9a69a.md`（ATK，RV32 SMP） |

两条都是 ATK 上跑的（RP2350，4 MiB）；`rpi-pico2-rv32-smp` 那次在更早的提交
`2a9a69a` 上，不是本卡的 `720f28d`。本次没有在 Pico 2 实物上重跑 soak。

## 与 ATK 代跑那轮的差别

| | ATK 代跑（`a5e4909`） | 本次实物（`720f28d`） |
|---|---|---|
| flash | 4 MiB W25Q32，`df` 3,670,016 | 8 MiB W25Q64，`df` 7,864,320 |
| `lfs_blocks` | 896 | 1920 |
| `accept33` | 需 `--df 3670016` | 不需要，默认期望即真值 |
| kthreadtest RV32 | 1 failure | 1 failure（同一条） |

两次的 accept 分数完全一致（20/22/21/23），说明镜像在 4 MiB 与 8 MiB 上行为一致；
RV32 的 #72 与 flash 容量无关。

## 复现

```sh
go run ./cmd/tos build -target rpi-pico2-smp
python scripts/rp2350/accept33.py --image rpi-pico2-smp \
    --uf2 build/tinyos-rpi-pico2-smp.uf2 -p COM19 --dd-rounds 100
python scripts/rp2350/kthread_accept.py --uf2 build/tinyos-rpi-pico2-smp.uf2 \
    --port COM19 --out build/kthread-rpi-pico2-smp.log --soft-bootsel
```

镜像 sha256（本次构建，`files.local` 已移出以符合发布口径）：

```text
ac2a9438e6c51ab9fcec31c64c9a98e3d0d05800205d42ec3554a29d08640386  rpi-pico2
925328b17c04f143a1a35b0e8d66f90db8d3ac2ad56683a3e9d4828fffc19393  rpi-pico2-smp
8bb28ed89df3c52ffcea9b5ad08901c8e52ac154345409771f91c08fe8b9359d  rpi-pico2-rv32
0484878331cf9d708d81d1cee0a846947e29b520df67bb7f64df608db96b52ab  rpi-pico2-rv32-smp
```
