# ATK 回归复核：board 重命名 + 芯片基线（2026-11-18）

```text
commit:            2d87ded（含 d8211aa 的 arch-flag 修复、e52ad7d 的镜像改名）
change:            复核 board 重命名（rp2350→atk-dnr2350am / rp2040→rpi-pico /
                   pico2→rpi-pico2）与 generic 芯片基线在真机上能否启动
baseline:          none（人工回归复核）
target/config:     atk-dnr2350am-smp / atk-dnr2350am-rv32-smp；另建临时
                   generic-flash 目标验证 rp2350-generic 基线（卡片记录后已删）
board/port/image:  ATK-DNRP2350AM; UART0 COM14; tinyos-atk-dnr2350am-smp.uf2 /
                   tinyos-atk-dnr2350am-rv32-smp.uf2
toolchain:         go 1.27.1, clang 23.1.4, qemu 11.1.2, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（2d87ded：build/vet/test -count=1）
T1:                PASS（全部 targets/*.conf 目标 + configs/rv32-fsro.conf）
T2:                smoke 4/4 PASS；soak 见日志
T3:                PASS——本卡即 ATK 真机复核
T4:                NOT RUN
logs:              build/atk-latest-arm-heartbeat.log,
                   build/atk-latest-rv32-heartbeat.log,
                   build/generic-flash-heartbeat.log,
                   build/atk-rename-bootfull.log（原始日志不入库）
```

## 背景

board 拆分（`1c5090b`）把 `board/rp2350` 改成 `rp2350-generic` + `atk-dnr2350am`，
但构建仍按 `Target.Board == "rp2350"` 选 arch flag。所有改名后的板子在**真机**上
启动即 panic（本复核先后在未修复镜像上抓到）：

```text
[C] vtor+sau ok
[mpu] window needs 5+5 blocks (budget 7): code is writable and executable for this task
[mpu] type=800 win=... regions=1
[pid 1] stack overflow (exception frame could not be stacked); no room to grow it; killing
init process exited with code 139 - shutting down
```

根因：RP2350 丢了 `-DTOS_ARM_MPU_V8`，走了 ARMv7-M 的 7-block MPU 路径 → 7-block
预算不够 → 退化成整窗 RWX → 任务栈保护错乱。实现侧 `d8211aa` 已改为按 chip 选
flag（ARM: `TOS_ARM_MPU_V8`/`TOS_ARM_SIO_SPIN`/`TOS_SYSCALL_TRACE`；RV32:
`TOS_COPY_DATA`），并加 `TestNoBoardNameSelectsBuildFlags` 守卫。

## 真机复核（修复后）

```text
# ARM
python scripts/rp2350/flash.py --boot-uart COM14 ...  (Ctrl-D -> BOOTSEL)
HEARTBEAT OK 6/6   target atk-dnr2350am-smp   port COM14

# LED / key / device table (ATK: LED=GP3 active-low, KEY/BOOT=GP2 active-low)
board: ATK-DNRP2350AM
LED off (gpio 3 1): wrote 1, reads 1
LED on  (gpio 3 0): wrote 0, reads 0
KEY gpio2: 0

# RV32
HEARTBEAT OK 6/6   target atk-dnr2350am-rv32-smp   port COM14
```

## generic 芯片基线（用户要求）

`board/rp2350-generic` 的 `fs_backing = embedded`（RAM disk），本设计不产 UF2。
为验证"generic 基线能作为任意 RP2350 板的启动基线"，临时建了一个
`base = rp2350-generic` + `fs_backing = flash` 的临时 board/target（**不入库，
测完删除**），构建 `tinyos-zz-tmp-generic-flash.uf2` 刷入 ATK：

```text
uptime: 7.071 s  tick=1000 Hz  cpus=1  cpu=0   # generic 是单核基线
HEARTBEAT OK 6/6   target atk-generic-flash   port COM14
```

**结论：`rp2350-generic` 基线镜像在 ATK 真机启动并跑通 6/6**——C1 的"芯片基线 +
板级组合"分层在真机上成立，generic 镜像确实与具体板无关。

## 没跑 / 未通过

- **RP2040（rpi-pico）/ Pico 2（rpi-pico2）真机**：`NOT RUN`，本轮只在 ATK
  （RP2350 ARM+RV32）上复核；Pico 2 的 A 轨仍 OPEN（见 issue #7）。
- **T4 压力 soak**：`NOT RUN`（重命名不改运行时代码，ATK 1000 轮证据见既有卡）。
- 临时 generic-flash board/target 已在测后删除，未入库。

## 备注

本次复核独立于实现侧 `2026-11-18-rename-boards-e52ad7d.md`，重点是从**真机**
确认 d8211aa 的修复生效，以及 generic 基线可启动。
