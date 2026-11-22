# Release 镜像瘦身：44 个 4 KiB littlefs 块（2026-11-22）

```text
commit:            79b9aad
change:            configs/release.conf 排除 24 个 demo/测试/旧 AMX 程序；默认
                   （带 trace 的验收镜像）不变
target/config:     rpi-pico2 + configs/release.conf；rv32(QEMU) + release.conf
board/port/image:  无板在机（release 只验证构建与 QEMU）
toolchain:         go 1.27.1 windows/amd64, clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test ./... -count=1
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — rv32 release 镜像 smoke（ls / cat readme.txt / ps）
T3:                NOT RUN — 没有上板
T4:                NOT RUN
```

## 数字

littlefs 的块是 4 KiB，**每个文件至少占一块**。同一份 `rpi-pico2` 默认镜像：

| | 文件数 | 块 | 占用 |
|---|---:|---:|---:|
| 默认（含 demo/测试） | 43 | 71 | 284 KiB |
| `release.conf` | 19 | 27 | 108 KiB |

ELF 字节总数只有 187 KiB，块开销把 1 KiB 的小程序也变成 4 KiB。release 去掉的
24 个文件（csa、pvm、coremark、pio、smp、smpbench、piperace、floattest、rx、
rawsink、samp、spin、swcost、rselftest、reloc、cpuprobe、crash、hold、prot、
stk、bench、deep、wheap、mem）共 **44 块 / 176 KiB**；`msh`/`sh` 和常用小工具
保留。`py` 本来就在 `defaultUserExclude` 里。

默认镜像不剪：`accept33.py` 要跑 `deep`/`prot`/`stk`/`smpbench`/`smp`/`dd`，验收
必须在带 trace 的默认镜像上做。

## 复跑

```sh
go run ./cmd/tos build -target rpi-pico2 -config configs/release.conf
go run ./cmd/tos build -target rv32 -config configs/release.conf
go run ./cmd/tostest smoke -target rv32 -commands "ls,cat readme.txt,ps"
```
