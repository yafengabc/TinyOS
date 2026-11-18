# 启动轨迹开关 + 提示符不再被打断（2026-11-18）

```text
commit:            6ca4f18
change:            ① [build] boot_trace 一个开关同时控制启动轨迹与 TOS_DIAG
                   ② CDC 配置通知 [U] 另起一行，且 shell 取过一行后永久停止
                   ③ configs/rpi-pico2-quiet.conf 作为“安静镜像”示例
baseline:          4d87f8b
target/config:     rpi-pico2（默认 + quiet 两种配置）；全 18 目标构建
board/port/image:  Pico 2（RP2350A，8 MiB W25Q64）；UART0 → CH343 COM19
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.2
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（gofmt / go vet ./... / go test ./... -count=1）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                PASS（tostest smoke rv32 / rv32smp / arm：3× SMOKE TEST OK）
T3:                PASS（Pico 2：默认构建启动到 shell；提示符不再被 [U] 打断）
T4:                NOT RUN（无 SMP 语义改动）
logs:              build/pico2-tracecheck.log（原始日志不入库）
```

## 改了什么

`boot_trace` 是一个「这张镜像要不要自我解释」的开关，默认 **开**：

| 值 | 效果 |
|---|---|
| 1（默认） | 平台打印 `[A]`…`[D]`、USB bring-up 各阶段、CDC 通知；`TOS_DIAG=1` 编入 wedge 报告 |
| 0 | 以上全部编译掉（`-DTOS_BOOT_TRACE=0 -DTOS_DIAG=0`），镜像只剩 banner |

`TOS_DIAG` 此前只是注释里写了 `[features] diag = true`，构建里从来没有把它变成宏，
所以那些 wedge 报告在任何镜像里都不存在；现在和轨迹共用一个开关，不再有两套说法。

menuconfig 里是「Boot traces + diagnostics」，配置文件里是 `[build] boot_trace = 0`。
默认保持开，因为 `scripts/rp2350/accept33.py` 依赖 `[B] clk=`、`[C] vtor+sau`、
`[D] cpu0`、`[E] usb` 这些标记。

## 显示修复

`[U] cfg=1` 原来直接接在 `tos> ` 后面（tick 里打印，且窗口长达 120 秒），看起来像乱码。
现在：

- 先输出 `\r\n`，永远另起一行；
- `tty_line_taken()` 一旦为真（有读者取走过一行，即 shell 已在处理命令）就不再打印。

修改前（同一块板）：

```text
TinyOS shell (pid 1).
tos> [U] cfg=1
```

修改后：

```text
TinyOS shell (pid 1).
tos> 
tos> uptime
uptime: 1.548 s (1548 ms)  tick=1000 Hz  cpus=1  cpu=0  pid=2
tos> 
```

## 构建期验证（字符串级）

```text
默认构建    [A]/[B]/[C]/[D]/[P1]/[E]/[U] 全部 PRESENT
quiet 构建  同上一律 absent；[SCR]/[pk] 等 diag 字符串也 absent
```

```sh
go run ./cmd/tos build -target rpi-pico2                      # 默认（带轨迹）
go run ./cmd/tos build -target rpi-pico2 -config configs/rpi-pico2-quiet.conf
```

## 没做

- T4 `NOT RUN`。
- ATK 真机 `NOT RUN`：本次改动对 ATK 同样生效（它的 `[A]` 历史乱码也不会再出现），
  但只验证了 Pico 2；ATK 接上后应补跑一次心跳级。
- 这块克隆板刷完 UF2 后 ROM **偶发不自动重启应用**（同一镜像有时自动启动、有时要按 RUN），
  与本次改动无关，属于实验台/ROM 行为。
