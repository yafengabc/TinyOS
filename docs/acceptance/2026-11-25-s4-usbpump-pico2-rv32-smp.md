# S4 USB 泵：Pico 2 RV32 SMP 板测（2026-11-25）

```text
commit:            本提交（§21 每核 trap 修复后的 SMP 泵板测记录）
change:            Pico 2 RV32 SMP 上跑 §19 的 usb_pump 清单；确认 §21 修复后
                   泵与用户任务可同时在两核运行
baseline:          1a41138（含 0cdb250 的每核 ktask trap 状态）
target/config:     rpi-pico2-rv32-smp + configs/rpi-pico2-rv32-usbpump.conf
board/port/image:  Raspberry Pi Pico 2；UART0 COM19、CDC COM27（2e8a:000a）；
                   build/tinyos-rpi-pico2-rv32-smp.uf2（2,097,664 B）
toolchain:         go 1.27.1 windows/amd64；Windows；串口 115200
host:              Windows / 本机
T0:                FAIL（仅 TestPinnedToolchainSources，pre-existing；docs-only）
T1:                NOT RUN
T2:                NOT RUN
T3:                PASS（SMP 主清单，见下；启动打印行是唯一观察缺口）
T4:                NOT RUN
logs:              build/pump-repro-smp-run.log、build/pump-fallback-smp-run.log
                   （不入库）
notes:             §21 已由 0cdb250 修复；SMP 与 UP 都跑过，UP 见另一张卡
```

## 结果

| 项 | 结果 |
|---|---|
| 启动 | `cpus=2`；CDC 0.28 s 出现并应答 `uptime` |
| 泵线程 | `/proc/ktask`：`0 6144 652 652 usbpump`（used=652 说明栈确实跑过） |
| 启动打印 | `[usb] pump thread pid N on cpu 0` **3/3 次未出现**；判为启动期两核控制台竞争（`kprintf` trylock）丢行，不是泵没起 |
| `ps` | 不列 `usbpump`（`process.c` 跳过 `is_ktask`，设计如此） |
| 长输出 | `/bin/rcc` 23,000 B → CDC 两轮均 23,138 B（bytes+lines）、138 CRLF、逐字节一致 |
| `crash` | UART0/CDC 各 101–102 B、内容一致、D/H=0，shell 回到提示符 |
| `smpbench` | `single 205 ms`，`dual 216 + 204 ms (wall 216)`，**1.89x**；泵在 core 0，core 1 未被明显拖慢 |
| `smp` | 2 CPU，两个 worker 都曾跨核迁移（`cpu 1 then 0`） |
| `heartbeat` | 6/6，tick 1000 Hz |
| 回退 | 默认 `-target rpi-pico2-rv32-smp`：无 `[usb] pump thread`，`/proc/ktask` 不存在，CDC 正常 |

## 执行的命令

```sh
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf
python scripts/rp2350/pump_repro.py --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2 \
    --uart-port COM19 --watch 40 --log build/pump-repro.log
python scripts/rp2350/heartbeat.py -p COM27 --dtr --target rpi-pico2-rv32-smp
go run ./cmd/tos build -target rpi-pico2-rv32-smp
python scripts/rp2350/pump_repro.py --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2 \
    --uart-port COM19 --watch 20 --log build/pump-fallback.log
```

CDC 清单与长输出用临时脚本 `build/cdc_console.py`、`build/longout_check.py`
（DTR=True，未入库）；启动打印缺失用 `build/reboot_capture.py` 连抓 3 次确认。

## 没跑的

- **`cat readme.txt` 单独一项**：SMP 这轮没有重复敲；`heartbeat` 的 `ls /bin` +
  `hello` 已覆盖同一文件系统/短写路径。
- **T1/T2/T4**：docs-only 改动，没有重建目标矩阵、没有跑 QEMU。
- **T0 的 pinned 源同步**：`TestPinnedToolchainSources` 在本 worktree 因
  `../tinyos-script` 与分支 pinned 拷贝不同步而红，与本次文档改动无关。
