# 窗口放置改为「buddy 优先、grain 兜底」：C3 + QEMU 验收（2026-11-22）

```text
commit:            80ee7d2 窗口放置两段式（#55 的 P1 复核项）
target/config:     esp32c3-generic（默认 rir 镜像）；rpi-pico2-rv32-smp；rpi-pico2；全 18 目标
board/port:        ESP32-C3（COM4）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4（宿主 harness）
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — tostest smoke rv32 + arm
T3:                PASS — C3：cover 九行一致，accept 24/24
T4:                NOT RUN
```

## 为什么改

`12d41c6` 把放置对齐统一成 grain，修好了 #55，但让 NAPOT/MPU 板的保护策略
依赖基址低位。用 `pmp_map.h` 对 `rcc` 的实测窗口（96 KiB，镜像 68 KiB，
wx +28K）算：

```text
0x20020000 (mod 64K 0x0000): SPLIT
0x20024000 (mod 64K 0x4000): SNAP
0x20028000 (mod 64K 0x8000): SPLIT
0x2002c000 (mod 64K 0xc000): SNAP
```

旧规则只会给出 64K 对齐基址（永远 SPLIT）；`12d41c6` 允许 0x4000/0xc000，
于是保护模式随内核体积变化。

现在：先按 buddy 对齐（不超过窗口的最大 2 的幂）放，放不下再退 grain。

- C3：128 KiB 窗口在 224 KiB 空隙里仍然走 grain 兜底，`#55` 不回归。
- Hazard3/MPU：`rcc` 在空隙允许时保持 64K 对齐 → SPLIT。

## 验证

宿主 harness（`internal/build/winlayout_test.go` + `testdata/win_layout_harness.c`）：

- C3 回归元组：旧规则放不下；新 `win_place` 返回 gap 起点且是 grain 边界。
- Hazard3 元组：`win_place(0x20024000, .., 96K, 16K)` 必须返回 `0x20030000`
  （64K 边界）；同时断言 grain-only 的结果 `0x20024000` 是更差的那个。
- 精确空隙/差一个 grain 的扫描、非 2 的幂对齐拒绝。

C3 真机（刷入 `esp32c3-generic` 默认镜像）：

```text
tos> csc /tests/cover.c /tests/cover.rir
csc: /tests/cover.c -> /tests/cover.rir
tos> rvm /tests/cover.rir
... 九行与 tests/onboard/cover.expected 一致
tos> rcc /tests/cover.rir /bin/cover
rcc: 4284 text, 4556 elf, 4603 tos, 199 labels
tos> cover
... 同上
```

`scripts/esp-c3/accept.py -p COM4 --no-reset`：**24/24 passed**。

## 未做

- Pico 2 RV32 真机未复测（无板）：`rcc` 在当前链接（`app_base=0x2001c000`）
  仍是 SPLIT；本次改动保证内核体积变化时也尽量保持 SPLIT，但 `prot` 用例
  需要上板确认（`accept33.py`）。
- `win_largest_free()` 的 `grain-1` 高报未处理（仅错误信息）。
