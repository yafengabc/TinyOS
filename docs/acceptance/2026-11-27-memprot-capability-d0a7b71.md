# 内存保护能力查询 + kthreadtest 的 fault 探针按平台（2026-11-27）

```text
commit:            d0a7b71
change:            新增 plat_has_memprot()（弱默认 0；ARM 的 M0+/NO_MPU 返回 0，M3/M33
                   返回 1；RISC-V 全部返回 1），/proc/cpuinfo 输出 `memprot: on|off`；
                   kthreadtest 的“地址 0 写入必须 fault”探针按它分流
baseline:          9518609
target/config:     18 目标；rv32 kthreads（configs/rv32-kthreads.conf）、arm kthreads
                   （configs/arm-kthreads.conf）、x86、esp32c3-generic
board/port/image:  ESP32-C3（COM4，真机）；QEMU rv32 / arm / x86
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...；go vet ./...）
T1:                PASS 18/18
T2:                PASS（rv32/arm 的 kthreads 镜像、x86；C3 真机 accept 42/42）
T3:                NOT RUN（RP2040 / RP2350 不在本机；这两块是这次改动的目标板）
T4:                NOT RUN
logs:              build/smoke-memprot-rv32.log、build/smoke-memprot-armoff.log、
                   build/c3-accept-memprot2.log（不入库）
notes:             #72 的代码侧；RP2040/RP2350 上的复验由板侧做
```

## 为什么

`kthreadtest` 里有两个依赖 fault 的探针：

1. 线程栈溢出 → 软件守卫报 `thread stack overflow ... killing`，join 拿 139。
   **这个不依赖 MPU**，M0+ 上也是过的。
2. `die_holding()` 写地址 0 → 期望线程 fault 死、join 拿 139，且它持有的 mutex
   被内核归还。M0+ 没有 MPU、`plat_task_protect()` 是空实现，写 0 不 fault，线程
   正常退出——探针的**前提**在这个端口上不成立。

测试不该按端口名猜，也不该默默跳过。内核现在把能力说出来：

```text
$ cat /proc/cpuinfo        # ESP32-C3
processor       : 0
isa             : rv32imc
board           : ESP32-C3 core (CH340K)
memprot         : on
```

```text
$ cat /proc/cpuinfo        # x86（弱默认）
memprot         : off
```

`kthreadtest` 读到 `off` 时打印

```text
kthreadtest: ok   join the thread that wrote to address 0 (memprot: off)
kthreadtest: N/A the holder died on its fault (memprot: off)
kthreadtest: ok   a dead holder released the mutex
```

——仍然检查"线程消失后 mutex 必须被归还"，只把"必须 fault"这条换成显式 N/A。
`memprot: on` 的端口继续严格要求 139，不放宽。

## 验证

```text
rv32 kthreads (PMP on)      memprot: on   kthreadtest all ok (0 failure(s))
arm kthreads，临时把能力改成 0（已撤回）
                            memprot: off  N/A 行出现，all ok (0 failure(s))
x86（弱默认）               memprot: off
C3 真机（flash --full + accept）          42/42
go test / go vet / 18 目标构建            PASS
```

arm 的那次是把 `plat_has_memprot()` 临时改成 `return 0` 编译后跑的，跑完立即撤回；
提交里的代码仍是 M3/M33 返回 1。这一步是为了在没有 RP2040 的情况下把 `off` 分支
真正执行一遍，而不是只靠读代码。

## 没跑的

- **RP2040 两镜像**：`kthreadtest` 应变成 N/A + `all ok`，由板侧复验（#66/#67）。
- **Pico 2 RV32**：`memprot: on`，探针仍是严格 139；如果还 FAIL，就不是"没有保护"，
  要按 fault 退出码/整组语义查（#72）。
- **panic / 保护缺失之外的内核回归**：本改动只加能力查询和一处测试分流。 
