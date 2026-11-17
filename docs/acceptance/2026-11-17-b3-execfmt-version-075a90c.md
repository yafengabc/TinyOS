# B-3 第二节：`exec_format.version` + `/proc/execfmt`（2026-11-17）

```text
commit:            075a90c
change:            exec_format / exec_image_format 追加 version 字段；lz4tos=1、elf32=1、
                   mocktos=1（测试容器）；新增 /proc/execfmt 打印两张表
baseline:          167db55
target/config:     rv32 / rv32smp / arm / x86（QEMU）；12 个目标 + fs_ro 构建；
                   configs/rv32-mock.conf（exec_mock 容器）
board/port/image:  无真机；QEMU 11.1.3（本地）+ CI ubuntu-24.04
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（2 skip，与改动前相同）
T1:                PASS 12/12（含 x86）+ configs/rv32-fsro.conf
T2:                PASS（arm/rv32/rv32smp/x86；命令含 cat /proc/execfmt）
T3:                NOT RUN（version 是元数据，不改加载路径；无板）
T4:                NOT RUN（soak 3 轮通过，1000 轮属 T4 未跑）
```

## 判据与结果

| 判据 | 结果 |
|---|---|
| 字段只追加、不重排 | `version` 加在两个结构末尾，四个初始化器改用具名初始化 |
| 两个 handler 填正确值 | `lz4tos = 1`（同时接受 V0 `0x0001` 与 V1 `0x0101`，1 是最新）、`elf32 = 1` |
| 容器/镜像两层注释讲清语义 | `kernel/exec_format.h`：`version` 是该 handler 能加载的最新修订号，`0` 保留给"未版本化的 handler" |
| 证据不止前缀假设 | `/proc/execfmt` 把两张表连名字、版本、note 一起打印 |

`/proc/execfmt`（默认 rv32 镜像）：

```text
container ver note
lz4tos 1 LZ4TOS envelope with 28-byte hint
image ver note
elf32 1 ET_DYN/ET_EXEC ELF32, segments relocated into the task window
```

`configs/rv32-mock.conf`（`exec_mock = true`）多出一行，证明表是被遍历的，不是写死的：

```text
container ver note
mocktos 1 test-only MOCKTOS envelope (raw ELF payload)
lz4tos 1 LZ4TOS envelope with 28-byte hint
```

## 没做

- **加载路径不读 `version`**：`probe()`/`load()` 的签名与语义不变；要拒绝某版本就在
  `probe()` 里拒绝（header 已经在手）。这样这个字段不会变成第二个"按前缀猜"的地方。
- **LZ4TOS 载荷与 `LZ4_WRAP_HEADER_V1`**：一个字都没动；名字仍然是 28 字节带 hint 头，
  不是格式版本号。
- 真机（T3）：字段是元数据，没有板子可验的东西；RP2350 的回归走 #15。

## 顺带

CI 的 QEMU smoke 命令表加了 `cat /proc/execfmt`（`075a90c` 的 `images.yml`），
`TESTING.md` §4.1 的 T2 命令同步。CI run `44024533427`：13 个 job 全绿，
`qemu smoke` 四条（rv32/rv32smp/arm/x86）+ `read-only` 都通过。
