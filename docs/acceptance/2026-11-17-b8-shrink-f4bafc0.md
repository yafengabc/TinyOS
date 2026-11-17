# B-8 小额收缩：heap 头 + ofile 路径哈希（2026-11-17）

```text
commit:            f4bafc0
change:            blk_t 16->8 字节；ofile_t.path[64] -> FNV-1a 哈希
baseline:          160ca72
target/config:     rv32smp（同一 config 对比）；esp32c3 / configs/esp32c3.conf
board/port/image:  ESP32-C3 DevKitM-1；COM4；build/esp32c3-full.bin
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows 11 / MSYS2 UCRT64
T0:                PASS
T1:                PASS（11/11 + x86）
T2:                PASS（smoke + soak 3/3）
T3:                PASS（C3 accept 24/24；soak 5 轮 resets 0 / errors 0）
T4:                NOT RUN
logs:              not kept
notes:             改前/改后用同一 target 配置对比 free
```

## 改前 / 改后（rv32smp，`free`）

```text
改前：kernel 705712   unused 113261392
改后：kernel 705488   unused 113261616     （省 224 B）
kernel heap 12288/262144 与 peak 768 不变
```

## 说明

- `blk_t` 头从 16 变 8：对齐靠"arena 16 对齐 + 大小按 8 取整 + 块前进 8+size"，
  `_Static_assert(sizeof(blk_t)==8)` 守住。
- `ofile_t` 用 FNV-1a 路径哈希比对 unlink；碰撞只会误判"仍被打开"并拒绝删除，
  不会误删。
