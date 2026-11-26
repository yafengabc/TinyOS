# RP2350：冷路径 diag 打印改为 out-of-line，默认镜像 −60 KiB（2026-11-27）

```text
commit:            0a62d78
change:            两个 RP2350 端口的 diag_char/diag_str/diag_u32/diag_hex/diag_kv
                   与 early_puts/early_put_hex 加 noinline + cold（DIAGFUNC）；
                   只去掉内联复制，不删任何 trace 字符串
baseline:          14e1770
target/config:     atk-dnr2350am-smp / -rv32-smp、rpi-pico2-smp（默认配置）；
                   atk + configs/release.conf（对照）
board/port/image:  无板（尺寸为构建产物测量；启动日志由板侧目视）
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...；go vet ./...）
T1:                PASS 18/18
T2:                PASS（rv32 / x86 smoke）
T3:                NOT RUN（ATK / Pico 2 的启动日志复验）
T4:                NOT RUN
logs:              build/t-cold-*.log（不入库）
notes:             #56 的 P1；与 #73（-Oz）无关，这里仍是 -O2
```

## 尺寸（构建产物，text 段）

| 镜像 | 之前 | 之后 | 差 |
|---|---|---|---|
| `atk-dnr2350am-smp`（M33，默认，trace 开） | 204,884 | **144,012** | **−60,872（−29.7%）** |
| `atk-dnr2350am-rv32-smp`（RV32，默认） | 134,318 | **123,966** | −10,352（−7.7%） |
| `rpi-pico2-smp`（M33，默认） | — | **137,672** | — |
| `atk-dnr2350am-smp` + `release.conf` | 134,612 | 134,612 | 0 |

`.bss` 不变（M33 137,208）。release 镜像不受影响，因为 `TOS_DIAG` 在那里本来就编译掉了；
这条只是让**默认/调试镜像**不再为同一段打印代码付两次体积。

结果：默认 M33 现在只比 release 大 **9.4 KiB**（144.0 对 134.6 KiB），trace 一条没少。

## 为什么安全

- `-O2` 把 `diag_*` 和 `early_*` 内联进每个调用点（#56 实测板级 TU 78.6 KiB ↔
  十个函数 out-of-line 后 19.1 KiB），字符串本身仍在内；
- `noinline` 只影响复制，不改变输出；`cold` 只影响放置（`.text.unlikely`）；
- XIP 关闭窗口不经过这些函数：QMI 探测用的是自己的 RAM-resident `qmi_diag_*`
  （RAMFUNC），RV32 的 park 超时诊断也在 QMI 进 direct mode 之前；
- `plat_diag_poll()` 从自旋等待里调用 `diag_char()`，多一次 `bl` 的代价可忽略。

## 和 `-Os` / `-Oz` 还差多少（同一棵树、同样带 noinline）

| `atk-dnr2350am-smp`，默认配置 | `.text` | 对 `-O2` |
|---|---|---|
| `-O2`（本提交） | 144,012 | — |
| `-Os` | 100,694 | −43,318（−30%） |
| `-Oz` | 88,144 | −55,868（−39%） |

所以 noinline 只拿掉了“重复的那一份”；`-O2` 与 `-Os` 之间剩下的 43 KiB 是**真的
代码生成差异**，按对象分组：

| 组 | O2 | Os | 差 |
|---|---|---|---|
| kernel（`kernel_*.o`） | 73,648 | 44,624 | −29,024 |
| board platform | 48,177 | 39,003 | −9,174 |
| 其它 | 28,750 | 25,084 | −3,666 |
| tinyusb + usb | 26,138 | 23,270 | −2,868 |
| littlefs | 20,620 | 18,000 | −2,620 |

最大的三个是 `kernel_process.o`（25,332 → 13,536）、`rp2350-generic_platform.o`
（18,220 → 10,140）、`kernel_libgcc.o`（4,602 → 1,480）。也就是说省下来的主要是
**调度/进程/平台/64 位辅助**的热路径——这正是 `-O2` 买的东西，不是可回收的浪费。

顺带一个反直觉的数据：`-Oz` 在 noinline 之后从 86,128 变成 88,144（+2 KiB）。
在 `-Oz` 下这些打印函数本身很小，内联反而是省体积的；所以如果以后启用 `-Oz`，
`DIAGFUNC` 应该按优化级别关闭（或只保留 `noinline` 不加 `cold`）。

## 结论

- 想再小只能换优化级别：`-Os` 是下一档（−43 KiB），但它还没在 RP2350 上验证过
  （`-Oz` 卡在 #73）。如果 `-Os` 能启动，它就是“体积/速度”的中间档，比 `-Oz` 更稳。
- 按库单独 `-Os` 不值得：littlefs + TinyUSB 合计只有 ~5.5 KiB，其余都在内核热路径。

## 验证

```text
go test -count=1 ./...   PASS
go vet ./...             PASS
18/18 targets            PASS
smoke rv32 / x86         SMOKE TEST OK
```

## 没跑的

- **ATK / Pico 2 启动日志**：`NOT RUN`。要目视确认 trace 行与值一致（应该逐行相同，
  只是打印实现不再复制）。若某行顺序变了，说明 `cold` 改到了不该改的函数。
- **性能**：CoreMark/smpbench 不受影响（`cold` 函数只在启动/诊断路径）。
