# T4：合并后的 RV32 SMP 30 分钟浸泡（2026-11-27）

```text
commit:            67cd572（merge exp/thread-sched 之后的 main）
change:            none —— 在合并后的 main 上跑 T4：soak_board -m 30（窗口/段/堆抖动）
baseline:          3f07c44（合并前镜像；本轮之前只在 ATK 与 2a9a69a 上做过 30 分钟）
target/config:     rpi-pico2-rv32-smp / defaults（features = pio, rir, kthreads, usb_pump）
board/port/image:  Raspberry Pi Pico 2（RP2350A，8 MiB W25Q64 克隆板）
                   UART0 → CH343 COM19；USB CDC = COM27
toolchain:         go 1.27.x windows/amd64, clang 23.1.4, python 3.14
host:              Windows, Git Bash
T0:                NOT RUN（本次只做 T4）
T1:                NOT RUN
T2:                NOT RUN
T3:                NOT RUN（烧写后 smoke 7/7；T3 见四镜像回归卡）
T4:                PASS —— soak_board -m 30：30 min，145 cycles，10 commands/cycle
logs:              build/soak-rv32smp-30min.log（原始日志不入库）
```

## 判据（`docs/TESTING.md` §6）

`soak_board.py` 每轮跑 `deep 3 / mem / wheap / piperace / ls / prot ×4 / ps`，判据是：

- `free` 前后的 kernel heap、free、tasks 数字**只允许峰值水位变化**；
- 出现一次窗口未归还、段陈旧（`deep`/`wheap` 报错）或 `free` 漂移就是失败；
- `piperace` 的校验和必须一致（`piperace: ok`）。

## 执行的命令与结果

```sh
# 镜像：main @ 67cd572
go run ./cmd/tos build -target rpi-pico2-rv32-smp          # UF2 2,097,664 B
python scripts/rp2350/flash.py --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2 \
    -p COM19 --uart-port COM19 --boot-uart COM19 --smoke   # 7/7 checks passed

# 先 1 分钟确认脚本与镜像配套
python scripts/rp2350/soak_board.py -p COM19 -m 1 --image rpi-pico2-rv32-smp
# PASS (1 min, 5 cycles, 10 commands)

# T4 本体
python scripts/rp2350/soak_board.py -p COM19 -m 30 \
    --image rpi-pico2-rv32-smp -l build/soak-rv32smp-30min.log
```

## 关键输出

```text
[1809.5s] rpi-pico2-rv32-smp       PASS  (30 min, 145 cycles, 10 commands)
[1809.5s]     | uptime: 5414.443 s (5414443 ms)  tick=1000 Hz  cpus=2  cpu=0  pid=6195
```

`free` 前后对比（脚本自带的 before/after 段）：

```text
                   total      used      free      peak
  kernel heap      32768         0     32768      3360     ← before
  kernel heap      32768         0     32768      3360     ← after

  SRAM            524288       512
  tasks           409600       400
  unused            7504         7
```

- **零漂移**：heap used 0、free 32768、tasks 400 KiB 前后完全一致；peak 3360 是历史
  峰值，没有随时间增长。
- `unused` 7504 B 是合并后镜像的形状：`usb_pump` 内核线程占了一个 4 KiB 窗口
  （合并前是 16 KiB）。
- 30 分钟内 `deep 3`、`mem`、`wheap`、`piperace`、四条 `prot`、`ps` 共 145 轮 × 10 条
  命令，没有一次窗口未归还或段陈旧，`piperace` 全部 `ok`。
- 结束 `uptime` 5414 s，与开始（约 3600 s）连续，期间没有复位。

## T4 的另一半：dd

同一轮 T4 的 flash 子项单独记录在
`2026-11-27-t4-rv32smp-30min-after-merge-67cd572.md`：1800 轮 × 32 KiB 写 + 读回 +
`rm`，1800/1800 通过，`df` 回到 4%。

## 没跑的

- T0/T1/T2/T3 本次没跑（纯 T4 补跑，镜像与 `67cd572` 构建产物一致）。
- ATK 未参与：`AGENTS.md` 的 soak 预算要求每个 ISA 一个 SMP 镜像，Pico 2 已覆盖
  RP2350 RV32；ATK 的四镜像 T4 在 `2026-11-26-atk-four-images-dd-soak-42f4abf.md`。
