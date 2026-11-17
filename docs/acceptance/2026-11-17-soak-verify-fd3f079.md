# T2 soak 修法复核：15 s 窗口 + 诊断（2026-11-17）

```text
commit:            fd3f079（harness 代码 a8d55b7；fd3f079 为诊断记录文档）
change:            tostest smoke 命令超时时点名命令与耗时；soak --commandwait-ms
                   默认 4000 -> 15000；-commands 拆分后 trim
baseline:          9e2057c（同机曾 1/3、2/3 波动，失败 target 随机）
target/config:     rv32 / rv32smp / arm（QEMU，soak 3 cycles）
board/port/image:  无真机；复用当前 build/ 内核 ELF（a8d55b7 未改内核）
toolchain:         go 1.27.1, qemu 11.1.2, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（go build/vet/test -count=1；cmd/tostest 1.109s）
T1:                NOT RUN（harness 改动，未改镜像）
T2:                PASS——soak 3/3，无 "no prompt" 诊断行
T3:                NOT RUN（板子本轮仅查 /proc/execfmt，见下）
T4:                NOT RUN
logs:              build/soak-fd3f079.log（原始日志不入库）
```

## 执行的命令与结果

```sh
python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm
```

```text
rv32     PASS  tasks      20250624     19776  (3 cycles)
rv32smp  PASS  tasks      20250624     19776  (3 cycles)
arm      PASS  tasks       3342336     3264  (3 cycles)
3/3 targets passed
```

日志中无 `smoke: no prompt after ...` 行，说明 3 个 target × 3 cycles 没有命令
静默超过窗口。

## 结论

复核机上剩余 T2 soak 失败定位为**慢主机上的静默命令超时**，不是代码回归：

- 同一份内核 ELF（`a271253` 起 rv32smp 为 `51DA5C33...`）在不同主机上成败不同；
- 失败 target 随运行变化（先后出现过 rv32smp、rv32 FAIL，arm 始终 PASS），
  说明是时机问题而非某个端口的实现缺陷；
- `3dae711`（有输出即滑动截止时间）+ `a8d55b7`（15 s 上限 + 点名命令）后
  本机与复核机均 3/3 PASS，修法成立。

## 顺带确认

- `cat /proc/execfmt`（rp2350rvsmp，COM14）：`lz4tos 1` 与 `elf32 1` 两份格式
  注册均在，与 `075a90c` 预期一致。
- T0 在最新 HEAD `fd3f079` 重跑 PASS。

## 没跑 / 未通过

- **T1 全目标构建**：`NOT RUN` 本轮；`313270b` 已做过 12 目标 + fsro 全过，
  `a8d55b7` 只改 test harness，不进任何镜像。
- **RP2040 / Pico 2**：`NOT RUN`，按 AGENTS.md 不作关闭依据。
