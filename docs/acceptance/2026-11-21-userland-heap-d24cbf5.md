# 用户态堆：first-fit 空闲链表（2026-11-21）

```text
commit:            d24cbf5（rebase 后；代码内容与 57153d3 相同）
change:            user/heap.c + heap.h 取代 bump 分配器：首次适配、切分、
                   与物理相邻空闲块合并，只在整张链表不够时 sbrk；kthread 的栈
                   改走 malloc；bench/wheap/mem 的 TOS_HEAP 各加 64 字节块头
target/config:     rv32-rir（QEMU rv32）、arm-kthreads / arm-rir（QEMU mps2）、
                   build all 的 18 个目标
board/port/image:  QEMU rv32 / mps2-an385；无 Pico 2 在机
toolchain:         go 1.27.1 windows/amd64, clang 23.1.4, goc32（native32）
host:              Windows 11 / EKSOFT
T0:                PASS — go test ./... -count=1；go vet ./...；goc32 与
                   native32 的 go test；goc32 -target rp2350 bench.c 样例编译
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — 见下（rv32 的 malloc 程序、mps2 的 kthread 测试、
                   goc32 的 benchgoc）
T3:                NOT RUN — 无 Pico 2
T4:                NOT RUN
```

## 宿主机用例

`internal/build/heap_test.go` 把真实的 `user/heap.c` 与一个拥有 256 KiB arena 的
mock `sbrk` 编在一起，输出：

```
ok (68392 bytes of arena used)
```

覆盖：8 字节对齐、尾块复用、切分（4 KiB 块服务两个 1000 B 请求）、合并（free
两块后 3000 B 不再增长）、增长、裸 `sbrk` 留下的空隙不跨过、窗口尽头返回 NULL、
double free 被忽略、`free(NULL)`、`free` 不收缩 break。

## QEMU

```sh
go run ./cmd/tos build -config configs/rv32-rir.conf
go run ./cmd/tostest smoke -target rv32 \
  -commands "wheap,mem 16,bench 1024 4,deep 3 0"
```

```
wheap: 12 KiB declared, 12 KiB taken, pattern intact
heap demo, pid=3, asking 16384 bytes
touched 4 pages, checksum=6
bench: 1024 B x 4 reps = 4 KiB per phase
deep: 3 levels, journal intact
SMOKE TEST OK
```

mps2 kthread 镜像（`configs/arm-kthreads.conf`）：

```
kthreadtest: all ok (0 failure(s))
prodcons: all bytes delivered, both threads blocked and woke
SMOKE TEST OK
```

goc32 的 `benchgoc`（`-target arm`）在 mps2 上重跑，四个校验和不变
（`46368 / 999850000 / 225792000 / 599994`），说明新分配器对 goc32 生成的镜像
同样可用。

## 没做 / 已知边界

- `realloc` / `calloc`；
- 线程安全（kthreads 共享窗口，两个线程同时 malloc 会竞争；`kthread_create`
  只在创建时分配，所以现有测试不触发）；
- 收缩 break（按 FreeRTOS `heap_4` 的取舍，只复用、不还）；
- T3：需要 Pico 2 上跑 `wheap`/`mem`/`bench` 与 kthread 测试后再补。
