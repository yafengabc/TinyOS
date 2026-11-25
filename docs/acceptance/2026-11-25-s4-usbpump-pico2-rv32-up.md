# S4 USB 泵：Pico 2 RV32 单核板测（2026-11-25）

```text
commit:            本提交（§19 描述修正 + 板测记录）
change:            记录 S4 usb_pump 在 Pico 2 RV32 单核上的板测结果；修正 §19 中
                   ps 列 ktask / crash 只走 UART0 / 镜像名 / 回退 target 的过时描述
baseline:          ff22596
target/config:     rpi-pico2-rv32 + configs/rpi-pico2-rv32-usbpump.conf（UP, usb_pump）；
                   回退项：go run ./cmd/tos build -target rpi-pico2-rv32
board/port/image:  Raspberry Pi Pico 2；UART0 COM19、CDC COM27（2e8a:000a）；
                   build/tinyos-rpi-pico2-rv32.uf2（2,097,664 B）
toolchain:         go 1.27.1 windows/amd64；Windows；串口 115200
host:              Windows / 本机
T0:                FAIL（仅 TestPinnedToolchainSources，pre-existing：本 worktree 的
                   ../tinyos-script 与分支 pinned 源不同步；其余包 PASS；docs-only）
T1:                NOT RUN（docs-only，未重建目标）
T2:                NOT RUN
T3:                PASS（Pico 2 RV32 UP，见下表）
T4:                NOT RUN
logs:              build/pump-repro-run.log、build/pump-fallback-run.log（不入库）
notes:             §21 修复前的 UP 基线；0cdb250 后 SMP 待补测
```

## 结果

| 项 | 结果 |
|---|---|
| 启动 / 泵 | UART0 全阶段正常；`[usb] pump thread pid 1 on cpu 0` 出现一次 |
| CDC | watch 开始后 0.28 s 出现，shell 应答 `uptime` |
| `/proc/ktask` | `0 6144 652 652 usbpump`（size 6144，used/hw 652） |
| `ps` | 不列 `usbpump`——`kernel/process.c` 跳过 `p->is_ktask`（设计如此，§19 已改） |
| 短输出 | `cat readme.txt`、`ls /bin` 正常 |
| 长输出 | `/bin/rcc` 23,000 B → CDC 两轮均 23,138 B（bytes+lines）、138 CRLF、逐字节一致 |
| `crash` | UART0/CDC 各 102 B、内容一致、D/H=0，shell 回到提示符 |
| `smp` / `smpbench` | UP：`smp` 报 1 CPU；两 hart 吞吐 N/A（等 §21） |
| `heartbeat` | 6/6，tick 1000 Hz |
| 回退 | 默认镜像无 `[usb] pump thread`，CDC 仍出现并应答 |

## 执行的命令

```sh
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf
python scripts/rp2350/pump_repro.py --uf2 build/tinyos-rpi-pico2-rv32.uf2 \
    --uart-port COM19 --watch 40 --log build/pump-repro.log
python scripts/rp2350/heartbeat.py -p COM27 --dtr --target rpi-pico2-rv32
go run ./cmd/tos build -target rpi-pico2-rv32
python scripts/rp2350/pump_repro.py --uf2 build/tinyos-rpi-pico2-rv32.uf2 \
    --uart-port COM19 --watch 20 --log build/pump-fallback.log
```

长输出与 CDC 清单用临时脚本 `build/longout_check.py`、`build/cdc_console.py`
（DTR=True；未入库）。

## 没跑的

- **SMP（`rpi-pico2-rv32-smp`）**：本卡跑在 §21 修复前的 UP 镜像上；`0cdb250` 已把
  ktask trap 状态改成每核、配置也切回 SMP，SMP 的同一清单待补测。
- **T1/T2/T4**：本次是 docs-only 改动，没有重建目标、没有跑 QEMU 矩阵。
- **T0 的 pinned 源同步**：`TestPinnedToolchainSources` 在本 worktree 因
  `../tinyos-script` 与分支 pinned 拷贝不同步而红，与本次文档改动无关；分支侧
  合并前需按 `pinned_test.go` 的提示把两边同步。
