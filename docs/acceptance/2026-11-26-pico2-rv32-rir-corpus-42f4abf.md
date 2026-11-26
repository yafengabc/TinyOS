# Pico 2 RV32 板上工具链语料（#69 结论，2026-11-26）

```text
commit:            42f4abf（板测镜像构建于 6a46477，另带本地语料脚本修复；
                   f9ab756 把两个 host-only 用例改为 INFO，42f4abf 过滤 rvm 的
                   result= 计时尾行——两者都不改变固件）
change:            #69：Pico 2 RV32 板的 csc -> rvm -> rcc 语料
baseline:          docs/OPEN-ITEMS.md §12.5 的 C3 宿主生成 / 板上执行结果
target/config:     rpi-pico2-rv32-smp（configs/rpi-pico2-rv32-rir.conf）、
                   rpi-pico2-rv32（build/rpi-pico2-rv32-rir-up.conf）
board/port/image:  Pico 2；UART0 COM19；两个 --source 镜像
toolchain:         go 1.27.3；clang 23.1.4；python 3.14.9
host:              Windows / 本机
T0/T1/T2:          NOT RUN（纯板测；语料本身无固件改动）
T3:                PASS（见下）
T4:                NOT RUN
logs:              build/thumb1/console-20261126-173707.log、
                   build/thumb1/console-20261126-173954.log、
                   build/thumb1/console-20261126-174211.log（本地，不入库）
```

## 命令

```sh
python scripts/rp2040/thumb1-prep.py --arch rv32 \
    --config configs/rpi-pico2-rv32-rir.conf --source      # SMP
python scripts/rp2040/thumb1-prep.py --arch rv32 \
    --config build/rpi-pico2-rv32-rir-up.conf --source     # UP
# BOOTSEL 刷对应 UF2 后：
python scripts/rp2040/thumb1-run.py -p COM19 --source --set all
```

## 结果

| 日志 | 镜像 | 原始汇总 | 两个 FAIL |
|---|---|---|---|
| `console-20261126-173707.log` | `rpi-pico2-rv32-smp` | strict 35/37 | `bigbody`、`hugeir` |
| `console-20261126-173954.log` | `rpi-pico2-rv32` | strict 35/37 | `bigbody`、`hugeir` |
| `console-20261126-174211.log` | `rpi-pico2-rv32-smp` | strict 36/37 | `hugeir`（本地放大 AST 池后的复核） |

其余 strict 用例全部逐行一致；`bigcode`（535 IR）与 `bigir`（1436 IR）在板上
`csc -> rvm -> rcc -> native` 四步都 PASS，1436 IR 的 `bigir` 生成 28,864 B RIR、
2,062 个标签，落在 RP2350 Hazard3 的 16 KiB 粒度 + 7 个 NAPOT 块预算内。
`eyeball 7 listed`，逐行输出见 transcript。

两条 FAIL 不是板子缺陷，按 issue #69 中 owner 的更正属于宿主专用：

- `bigbody`：约 450 AST 节点，板上 `csc` 的 `NODE_MAX 256`（OPEN-ITEMS §12.1）；
- `hugeir`：59,112 B RIR2，板上 `rcc` 的 `RIR_MAX 40,960`（OPEN-ITEMS §12.2）。

`f9ab756` 已把这两条在 `--source` 模式改为 `INFO host-only`，预编译模式仍覆盖它们。
按更正后的口径，上表原始 35/37 即 **strict 35/35**；`42f4abf` 再过滤 rvm 附加的
`result=...` 计时尾行，使比较只剩程序自身输出。

## 未跑 / 限制

- UP 镜像只跑了一轮；SMP 跑了两轮，结果一致（35/37 → 36/37 的差异只来自本地临时
  放大 `NODE_MAX`，不是语料输出变化）。#69 owner 的最新更正要求"重跑一遍"而不是
  每个镜像两轮；若坚持逐镜像两轮，需要把 Pico 2 板换回来补一轮 UP。
- 本地为诊断放大过 `user/csc/csc.c` 的 `NODE_MAX` 与 `user/rcc/rcc.c` 的
  `RIR_MAX`，已在切到 ATK 板前全部 `git checkout` 回退，未进入任何提交。
- ATK 板上的同族结果不能替代本卡；RP2350 的关闭门槛是 Pico 2。
