# `tools` 再降一档 span：`dd` 去掉第二个 4 KiB 缓冲（2026-11-28）

```text
commit:            92fdab1  tools: dd verifies without a second 4 KiB buffer
change:            dd 的读校验改成边走边重新生成期望字节，删掉
                   `want[MAX_BS]`（4 KiB .bss）和 pattern_fill 里没人用的校验和
target/board:      ESP32-C3（COM4，实机）；18 个构建目标；QEMU smoke
toolchain:         go 1.27.x；clang/ld.lld 23.1.4
T0:                PASS（go test ./...、go vet）
T1:                PASS 18/18
T2:                PASS smoke rv32 / rv32smp / arm（命令含 dd 写读）
T3:                C3 PASS（accept 42/42）；rp2040 / rp2350 NOT RUN（板子未接）
T4:                NOT RUN（只改一个用户态工具，不碰调度/flash 压力路径）
logs:              build/t-dd-*.log（本地，不入库）
```

## 为什么是缓冲而不是拆命令

`tools` 的 applet 代码（rv32，20 个文件）是 **6,860 B text + 2,080 B rodata**，
最大的单个 applet 是 `grep`（878 B text），`dd` 只有 730 B。真正占地方的是
`dd` 的 `.bss`：`buf[4096]` + `want[4096]`，一共 8 KiB，比全部代码还大。

读校验并不需要第二块缓冲：

```c
for (int i = 0; i < n; i++) {
    state = state * 1664525u + 1013904223u;
    if (buf[i] != (uint8_t)(state >> 24)) { ... mismatch ... }
}
```

和原来的 `pattern_fill(want, &state, n)` 完全同一条 LCG 序列；`pattern_fill`
返回的校验和两边都没人用，一并删掉。

## 结果

| | 改前 | 改后 |
|---|---|---|
| C3 / rv32 span | 20 KiB（hi 0x4d20） | **16 KiB**（hi 0x3d00） |
| C3 窗口 | 28 KiB | **24 KiB** |
| ARM span | 24 KiB（hi 0x5470） | **20 KiB**（hi 0x4450） |
| ARM 窗口 | 32 KiB | **28 KiB** |

打了个 class 的翻身仗：4 KiB 的 .bss 正好压在 20/24 KiB 那一档上。

## 验证

```text
tos> dd of=/d bs=4096 count=3   -> wrote 12288 bytes to /d
tos> dd if=/d bs=4096 count=3   -> read 12288 bytes from /d, pattern verified
tos> wfile /bad not the pattern at all
tos> dd if=/bad bs=64 count=1   -> data mismatch at block 0 byte 0
tos> accept.py -p COM4          -> 42/42 passed
```

T0/T1 18/18/T2 smoke 三个架构均过。

## 现在离下一档还有多远

- C3：hi 0x3d00（15,616 B），到 12 KiB span 还差 **3,328 B**；
- ARM：hi 0x4450（17,488 B），到 16 KiB span 还差 **1,104 B**。

ARM 上再省 ~1.1 KiB 就会再降一档（20 → 16 KiB span，窗口 28 → 24）；但拆命令
不划算：`grep` 拆出去也就 ~1 KiB 代码，却要多一个 4 KiB flash block 和一个
独立窗口。C3 要降档得省 3.3 KiB，更该从共享的 libc/静态缓冲上找，而不是拆。
