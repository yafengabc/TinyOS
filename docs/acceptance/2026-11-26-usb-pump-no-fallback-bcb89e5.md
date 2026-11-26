# USB 泵：删除 tick 回退，创建失败直接 panic（2026-11-26）

```text
commit:            bcb89e5
change:            三个端口删除 tick/idle 泵与 TOS_USB_PUMP_FALLBACK；usb_pump 不再是
                   feature（有 kthreads 的镜像必然创建泵线程）；创建失败 panic；
                   ktask 池尺寸改跟 TOS_KTHREADS（4096 / 12288，与原来相同）
baseline:          fc4bc80
target/config:     18 目标；esp32c3-generic（T2）+ QEMU arm/rv32/x86
board/port/image:  RP2350 / RP2040 板不在本机（成功路径与原来相同，失败路径见下）
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...；go vet ./...）
T1:                PASS 18/18
T2:                PASS（arm/rv32/x86 smoke；emutest 0 missing）
T3:                NOT RUN（RP2350/RP2040 无板；C3 不涉及 USB 泵）
T4:                NOT RUN（同上）
logs:              build/t-nofb3-*.log、build/emutest-nofb.log（不入库）
notes:             #68 关闭；#57 的板级 issue 从 12 个变 11 个
```

## 为什么

回退有两个作用，两个都不成立：

1. **编译期**：`usb_pump = false` 的镜像走 tick/idle 泵。这是迁移前的实现，留在树里
   等于维护两条控制台路径；RP2040 上那次迁移正是在 tick 里把控制器状态机跑在中断
   上下文，才需要改成线程。
2. **运行期**（RP2350）：`TOS_USB_PUMP_FALLBACK` 在线程创建失败时保留 idle/PendSV
   钩子。它让"泵没起来"的板子仍然在控制台上应答——板测看到的是一个正常工作的
   系统，正好把要暴露的失败盖住。

现在：有 `kthreads` 的镜像必然创建泵线程；创建失败 → `panic("...no owner")`。
`panic()` 走 UART0 直写（不需要泵），板测要么看到这行字、要么看到完全沉默，不会
再看到"好像正常"的假象。

## 结构改动

- `board/rp2350-generic/platform.c`、`board/rp2350rv-generic/platform.c`：
  删 `usb_idle_poll()` 与 `g_idle_poll` 赋值；`plat_start_tasks()` 去掉
  `#ifdef TOS_USB_PUMP` 与 `TOS_USB_PUMP_FALLBACK`，创建失败 `panic()`。
  `g_usb_ready == 0`（控制器本身没起来）仍只打印一行、不 panic——那不是泵的失败，
  杀掉板子只会让它更难救。
- `board/rpi-pico/platform.c`：tick 里的 `usb_cdc_poll()`/replug/trace 整段删除；
  泵线程无条件编译；创建失败 `panic()`。
- `board/*/board.conf`：`usb_pump` 从 features 移除；`configs/rpi-pico2*-usbpump.conf`
  变成"只有 target"的最小 E3 镜像（保留作历史入口）。
- `kernel/process.c`：`KTASK_SLOTS`/`KTASK_ARENA_BYTES` 的 `TOS_USB_PUMP` 分支改成
  `TOS_KTHREADS`。数字故意不变：pump-only 镜像 1 槽 / 4096 B，ktest 3 槽 /
  12288 B——和原来的 `usb_pump` 构建完全相同，所以 RP2350/RP2040 镜像尺寸不变。
- `kernel/procfs.c`：`/proc/ktask` 的编译条件去掉已不存在的 `TOS_USB_PUMP`。

代价：只开 `kthreads`（没有泵端口，例如 C3 kthreads 测试配置）的镜像拿到 4096 B
arena 而不是 1024 B，多 3 KiB `.bss`；发布镜像没有 kthreads，不受影响。

## 关键输出

```text
18/18 targets build
go test -count=1 ./...            PASS
go vet ./...                      PASS
smoke arm / rv32 / x86            SMOKE TEST OK
emutest                           EMU TEST OK (boot 84 ms, reboot 155 ms, 0 missing)
```

## 没跑的 / 风险

- **RP2350 / RP2040 真机**：本机没有板子，`NOT RUN`。成功路径与 `bcb89e5` 之前逐行
  相同（同样的池尺寸、同样的线程体），但**失败路径（panic）没有板测**。
- **制造失败**：要验证 panic 需要一个"让 `process_ktask_create` 失败"的测试钩子
  （例如临时把 arena 调小），目前没有；这条留在 `docs/OPEN-ITEMS.md` §22。
- **泵的压测**：不再有回退镜像，30 分钟浸泡（#60–#65）与 accept 的长输出直接跑在
  唯一的泵线程上，泵死掉 soak 会因无提示符失败——这就是压测。
