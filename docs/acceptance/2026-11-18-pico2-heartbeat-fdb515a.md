# Pico 2 标准流程验收：flash smoke 7/7 + 心跳 6/6（2026-11-18）

```text
commit:            fdb515a（含 cff442f 的启动修复、6ca4f18 的轨迹开关）
change:            无代码改动；按 TESTING.md §5.4 对 rpi-pico2 跑标准流程
baseline:          4d87f8b
target/config:     rpi-pico2（Cortex-M33 单核，默认配置，boot_trace 默认开）
board/port/image:  Pico 2 克隆板（RP2350A，8 MiB W25Q64）；UART0 GP0/GP1 → CH343 COM19；
                   build/tinyos-rpi-pico2.uf2
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（go test ./... -count=1；go vet ./...）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                PASS（tostest smoke rv32/rv32smp/arm：3× SMOKE TEST OK）
T3:                PASS（Pico 2：flash smoke 7/7；心跳 6/6）
T4:                NOT RUN（不做长时压测）
logs:              build/pico2-tracecheck.log（原始日志不入库）
```

## 命令与结果

```sh
# 1) 标准刷写 + 启动日志校验（BOOTSEL 后由脚本等盘符、复制 UF2、等提示符）
python scripts/rp2350/flash.py --uf2 build/tinyos-rpi-pico2.uf2 --smoke -p COM19
#   → 7/7 checks passed
#     （boot / uptime / fs / user program / gpio / reboot；reboot 后重新回到 tos>）

# 2) 心跳级 6 项（TESTING.md §5.0）
python scripts/rp2350/heartbeat.py -p COM19 --target rpi-pico2
#   → HEARTBEAT OK 6/6
```

心跳逐项：

```text
PASS  1 boot to a shell           prompt seen
PASS  2 time advances 1 s         delta=1.114 s, wall=1.115 s
PASS  3 filesystem read           ls /bin
PASS  4 user program              hello
PASS  5 filesystem write-back     wfile/cat/rm
PASS  6 fault isolation           crash returned to prompt
```

`flash.py --smoke` 的 7 项（脚本输出节选）：

```text
cat /proc/uptime        0.372
gpio                    gpio: 30 pins, 0..29
reboot                  reboot: resetting the machine
                        ... TinyOS shell (pid 1).  tos>
7/7 checks passed
```

## 新增/修改的仓库内容

- `scripts/rp2350/heartbeat.py`：RP2350 的心跳脚本（与 RP2040 那份同样的 6 项），
  `--target` 只用于记录；README 规定的 `scripts/<board>/heartbeat.py --port … --target …`
  形式现在对 RP2350 也成立。
- `TESTING.md` §5.1/§5.4：Pico 2 从“暂不支持”改为可启动目标，写明刷写/心跳命令、
  期望的 `[A]..[E]` 标记与未覆盖项（屏幕、TF、PIO 灯带、24 项表）。
- `OPEN-ITEMS.md` §1b：关闭“Pico 2 启动失败”，记录根因与当前边界。

## 没做（NOT RUN）

- **ATK-DNRP2350AM 真机**：本次改动涉及 generic RP2350 平台代码，ATK 需补跑
  T3（Cortex-M33 与 Hazard3 RV32 SMP 两个镜像）；板子不在本次实验台上。
- **Pico 2 SMP / RV32 镜像真机**：只构建通过。
- **T4 压测**：未跑 `soak_board.py`（需要长时占用板子）。
- **目视项**：GP25 上的 1 Hz 心跳不亮（`led_set()` 仍硬编码 GP3，issue #27），
  所以“内核活着”目前只能用串口/`uptime` 判断；PIO 灯带在 Pico 2 上不存在。
- **实验台观察**：这块克隆板刷完 UF2 后 ROM 偶尔不自动重启应用，需要按一次 RUN；
  同一镜像有时又会自动启动，属于 ROM/实验台层面，未计入固件结论。
