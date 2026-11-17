# smoke 超时诊断 + soak 窗口 15 s（2026-11-17）

```text
commit:            a8d55b7
change:            tostest smoke 在命令超时时打印命令名与耗时；soak 的
                   --commandwait-ms 默认 4000 -> 15000；-commands 拆分后 trim
baseline:          9e2057c
target/config:     QEMU rv32/rv32smp/arm（本机）；CI 同套命令表
board/port/image:  无真机；目的是让复核机上的 soak 失败可定位
toolchain:         go 1.27.3, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（go build/vet/test）
T1:                NOT RUN（未改镜像）
T2:                PASS（soak 3/3；arm smoke 标准命令表 OK）
T3:                NOT RUN
T4:                NOT RUN
```

## 为什么

复核机在 `313270b` 上（含 `3dae711` 的滑动等待）soak 仍 1/3，但 transcript 只说
"输出丢了"。harness 其实知道是哪条命令超时、等了多久——只是把它丢了。现在：

```text
smoke: no prompt after "piperace" (2.1s)
--- smoke: 1 command(s) never echoed a prompt:  piperace
```

soak 的上限同时 4 s → 15 s：等待在提示符回来时立刻结束，所以这个数字只在命令真的
慢/卡住时才花时间；而"静默超过窗口"正是让下一条命令插进上一条输出的原因。

反向验证（本机）：`-commandwait-ms 100` 跑 `ps, piperace, uptime`，诊断行按预期打出
`piperace` 与 2.1 s，且 `SMOKE TEST OK`（历史行为：超时不算失败）。

## 没做

- 没有把"某条命令超时"升级成失败：有些命令本来就长，历史行为是继续跑；诊断行足够定位。
- 没在复核机上复测——这一条要等他们下一次 soak。
