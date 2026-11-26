# ATK 四镜像 accept33 全表（#70，2026-11-26）

```text
commit:            ca0df04（accept33 与固件未改；四个 UF2 于 6a46477 构建，均晚于 0252af9）
change:            #70：ATK-DNRP2350AM 四个镜像各跑一遍 accept33.py 全表
baseline:          2026-11-19 ATK 24 项验收；Pico 2 四镜像的 accept33 记录
target/config:     atk-dnr2350am、atk-dnr2350am-smp、atk-dnr2350am-rv32、
                   atk-dnr2350am-rv32-smp，全部默认配置
board/port/image:  ATK-DNRP2350AM；UART0 CH343 COM14；四个镜像逐个刷入
toolchain:         go 1.27.3；clang 23.1.4；python 3.14.9
host:              Windows / 本机
T0/T1/T2:          NOT RUN（纯板测；镜像已由 6a46477 构建）
T3:                PASS ×4（见下）
T4:                NOT RUN（本轮只跑 accept33；30 分钟 soak 另见
                   2026-11-26-atk-four-images-dd-soak-42f4abf.md）
logs:              build/accept33-<image>.log、build/testlogs/accept33-rest.log
                   （本地，不入库）
```

## 结果

| 镜像 | accept33 | 首字节 / 提示符 |
|---|---|---|
| `atk-dnr2350am` | **19/19 PASS** | 0.46 s / 0.46 s |
| `atk-dnr2350am-smp` | **21/21 PASS** | 0.41 s / 0.41 s |
| `atk-dnr2350am-rv32` | **20/20 PASS** | 0.41 s / 0.73 s |
| `atk-dnr2350am-rv32-smp` | **22/22 PASS** | 0.42 s / 0.68 s |

全部 `0 flash-then-boot without RESET` 通过（首字节 ≤ 5 s），无 FAIL 行；
`df total 3670016`、ps 的 msh wink、保护用例、`deep 3` 连同 `journal intact`、
SMP 的 `smpbench >= 1.8x` 均通过。

命令原文（每个镜像重复）：

```sh
python scripts/rp2350/bootsel_poke.py COM14 25
python scripts/rp2350/accept33.py --image <image> --uf2 build/tinyos-<image>.uf2 -p COM14
```

## 工具坑：必须先让板子进 BOOTSEL

直接跑 `accept33.py --uf2 …` 会卡在 `waiting for the BOOTSEL drive` 30 s 后 exit 2——
脚本不会自己发 Ctrl-D。外部刷机再 `--no-flash` 虽然能跑完 24 项，但启动时串口没开着，
`boot banner` / `[B]..[E]` / RV32 的 `[A] pre-pll` 快照全部丢失，误报 3–5 项 FAIL。
正确做法是**先**用 `bootsel_poke.py COM14 25` 让板子停在 BOOTSEL，**再**启动
`accept33.py --uf2`：脚本在串口已打开的状态下完成刷机，首字节在 0.4 s 内落进日志。

ATK 不是 RP2350 的关闭门槛，本卡只对应 #70，不能当 Pico 2 的证据。
