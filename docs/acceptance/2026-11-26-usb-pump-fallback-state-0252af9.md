# USB 泵：fallback 是可选状态，默认只有泵线程（2026-11-26）

```text
commit:            0252af9（取代 bcb89e5）
change:            默认镜像 = 泵线程唯一 owner，创建失败 panic；`usb_pump_fallback = true`
                   是可选补充状态：泵启动失败时安装 idle/PendSV 钩子（RP2040 是 tick 继续轮询），
                   控制台继续工作。两者不会并行：只有创建失败后才进入 fallback。
baseline:          fc4bc80（bcb89e5 之前）
target/config:     18 个默认目标 + rpi-pico / rpi-pico2-smp 各一个
                   `usb_pump_fallback = true` 的构建
board/port/image:  无板（RP2350/RP2040 都不在本机）
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...；go vet ./...）
T1:                PASS 18/18；两个 fallback 变体也编译通过
T2:                PASS（QEMU arm/rv32 smoke；emutest 0 missing）
T3:                NOT RUN（默认泵路径与 fallback 路径都没有板测）
T4:                NOT RUN
logs:              build/t-fbfinal-*.log、build/t-fbstate-*.log、build/emutest-fbopt.log（不入库）
notes:             bcb89e5 的“删除 fallback”被本提交取代；release.conf 暂不打开 fallback
```

## 两种配置分别是什么

| | 默认（开发/测试镜像） | `usb_pump_fallback = true`（可选） |
|---|---|---|
| 唯一 owner | 泵线程 | 泵线程 |
| 创建失败 | `panic()`，UART0 直写一行；板子停下来 | 进入 **fallback 状态**：RP2350 安装 idle/PendSV 钩子，RP2040 让 tick 继续轮询；控制台继续工作 |
| 日志 | `[usb] pump thread NOT started ...` + panic | `[usb] pump thread NOT started ...` + `[usb] fallback state: ...` |
| 用途 | 开发/测试：失败必须暴露，泵被充分施压 | 发布可选兜底；或确实腾不出 ktask 槽的镜像 |

“fallback 是补充状态”不是措辞问题：钩子/`g_pump_thread` 在创建**之前**就关掉，只有
创建失败后才重新打开。所以正常情况下不存在第二个轮询者，板测看到的就是泵线程在跑。

## 为什么默认不开 fallback

开发/测试阶段要的是“泵坏了就炸”：30 分钟 soak（#60–#65）和 accept 的长输出全部
跑在唯一 owner 上，泵死掉 soak 会因为拿不到提示符直接失败。fallback 一旦默认打开，
同样的失败会表现为“控制台还能用”，正是要避免的假通过。

发布阶段的兜底写在 `configs/release.conf` 的注释里：**默认仍然关着**，因为 fallback
状态还没有一次板测；打开它只需要在 release.conf 里加 `usb_pump_fallback = true`，
前提是先补一次板测（`docs/OPEN-ITEMS.md` §22）。

## 验证

```text
18/18 targets build                          默认配置
rpi-pico         + usb_pump_fallback=true    build ok
rpi-pico2-smp    + usb_pump_fallback=true    build ok
go test -count=1 ./...                       PASS
go vet ./...                                 PASS
smoke arm / rv32                             SMOKE TEST OK
emutest                                      EMU TEST OK (boot 91 ms, reboot 151 ms, 0 missing)
```

## 没跑的

- **两种路径的真机**：RP2350 / RP2040 都不在本机，`NOT RUN`。默认路径的代码与
  `bcb89e5` 之前相同（同样的线程、同样的池尺寸）；fallback 状态是新增的，没有板测。
- **panic 路径**：没有“制造创建失败”的钩子，`NOT RUN`。
- **release.conf 打开 fallback**：等上面那条板测有结果再说。
