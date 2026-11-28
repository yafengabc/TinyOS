# 栈区设计：每线程独立栈窗，无 guard（草案，2026-11-28）

状态：**原型 2 已实现（2026-11-28：C3、RP2350 M33、RP2350 Hazard3）**：栈区在
用户区顶部，初始 16 KiB，按 16 KiB 步进向下增长、空闲时缩回；数据窗从底部
向上分配，上界是 `stack_area_low - 4 KiB`。主线程和 kthread 的栈都从栈区分配，
`join`/`exit`/进程退出归还；每窗口的 4 KiB guard 已删除，`ps` 的 `wink` 是数据窗、
`stk/sz` 是栈窗。`PLAT_STACK_AREA` 是 chip/ISA baseline 的事实（C3 16 KiB、
RP2350 两个 baseline 各 16 KiB），具体板子通过 `base =` 继承。RP2040 和
x86 仍是 `PLAT_STACK_AREA = 0` 的旧路径，canary 保留；RP2350 的 T3 等上板。
数字以 C3（rv32，4 KiB grain）为准，RP2350 的 4/16 KiB grain 在验收卡里。

## 1. 目标

- 线程栈不再从 `TOS_HEAP` 里 `malloc`：堆只装数据，大小可预测，回收确定；
- 每个栈有自己的窗口（4/8/16 KiB，按 `TOS_STACK`），由**默认拒绝**提供边界，
  删除每个窗口的 4 KiB guard；
- 线程栈由 PMP/MPU 保护：下溢立即 fault，而不是等下一次 syscall 的 canary；
- 大帧跨栈不再可能：栈下面任何未映射地址都 fault，`sub sp, 8K` 也一样。

## 2. 地址空间布局

```
 用户区顶部
 ┌───────────────┐
 │ stack N       │  4/8/16 KiB，2 的幂，按大小对齐，向下分配
 │ ...           │
 │ stack 0 (main)│
 ├───────────────┤
 │ 4 KiB gap     │  整个系统只保留这一个空洞
 ├───────────────┤
 │ data windows  │  span + heap（每个进程一个）
 └───────────────┘
```

- 栈区内相邻的栈在**别的线程的视图里未映射**，所以一个栈下溢进入邻栈也 fault；
- **两头分配**：数据窗从用户区一端分配，栈区从另一端分配，中间是两者共用的
  自由空间。栈区一开始只占 **16 KiB**（4 个 4 KiB 栈），不够时按 16 KiB 向下/向上
  扩展；栈都释放后可以缩回 16 KiB。不会像固定 64 KiB 那样永久吃掉最大窗口；
- 栈区与数据区之间保留**一个 4 KiB 空洞**（栈在数据区上方时），保证最低的栈
  下溢不会落进本任务的数据窗；栈区内部相邻的栈在别的线程视图里未映射，默认
  拒绝就够；
- 栈窗大小仍是 2 的幂（`TOS_STACK` 的要求），对齐到自身大小，满足 NAPOT/MPU；
- 原型 1 为了先把 buddy/映射/回收跑通，用了“底部固定 64 KiB”；那只是中间步骤，
  不作为结论。

## 3. 保护视图

当前任务的 PMP/MPU 只映射：

```
code        [win_base, wx)
data        [wx, brk)            （heap 随 sbrk 往上长）
stack       当前线程的栈窗
(+ 平台的 trap 向量 / 设备项)
```

- 主线程的栈也在栈区，和 worker 一样，不再是“数据窗顶部”；
- 线程切换：只重写 **stack 这一条**（数据/代码不变）；
- 进程切换：整表重编程（和现在一样）；
- **ARM 的 landing 只留硬件帧（32 B）**：异常压栈失败就是 MSTKERR，直接杀；
  不再有“增长”，0xF00 的旧 strip 没有存在意义。栈区下这个 strip 会直接写进
  下一块栈（M33 实测 `deep 4` 到 ~7.33 KiB 才被杀，邻栈已被踩），所以
  `ARM_STACK_LANDING` 从 0xF00 缩到 `ARM_HW_SIZE`，其余 hole 全部是 guard。

## 4. 生命周期与回收

- `exec`：分配数据窗（`span + heap`）+ 栈窗（`TOS_STACK`，缺省 4 KiB）；
  argv/初始帧放在栈窗顶；任一步失败要把另一个窗口还回去；
- `thread_create`：内核从栈区分配栈窗，`thread_t` 记录 base/top；
- `thread_join` 成功、`thread_exit`：**立即**归还栈窗；没人 join 的线程
  保留栈和槽到进程退出（与 POSIX 僵尸一致，不是泄漏）；
- `exec` 失败、进程被杀、组长退出：整组的栈窗一并回收；
- **栈区用 buddy 分配器，不用 bump / 水位指针**：块大小就是 `TOS_STACK`
  的 2 的幂（4/8/16 KiB，QEMU 板 32 KiB），申请时按需二分、释放时与伙伴合并。
  这样每次 create/join 都会回到空闲链，不会出现 sbrk 那种“只升不降”；
  混合大小也不会像 first-fit 那样把 16 KiB 洞切成碎片；
- 计数器：`SYS_STACKINFO`（`struct stackarea`）暴露 reserved / live / peak，
  `free` 和 `kthreadtest` 用它判断“1000 次 create/join 后回到基线”；
- 耗尽：`thread_create` 返回 -1，不部分分配、不泄漏；
- 数据窗沿用现有 `win_alloc`，上界跟着栈区当前边界走（`area_low - 4 KiB`）
  并在扩展时重算；没有改成“从高地址端向下放”，因为那会动 `winlayout` 的
  既有断言。代价是扩展前必须扫描窗口表，确认新下沿高于最高数据窗 + 4 KiB；
- 栈区扩展：容量按 16 KiB 步进；扩展前确认新范围没有被数据窗占用。收缩：
  当最外侧的 16 KiB 全部空闲时退回，释放的地址空间重新可被数据窗使用；
  这两个操作都在 `proc_lock` 下，和栈窗的分配/释放一起；
- 数据窗和栈区都在同一个 `win_alloc` 扫描里，重叠是硬错误（启动自检和
  `thread_create` 失败路径都要覆盖）。

## 5. 语义（明确写进 `lib.h`）

- **线程栈是线程私有的**。跨线程共享数据用 global/static；
- 把栈上对象的地址传给另一个线程并在那边访问，会 fault——不是未定义行为，
  是设计上的拒绝；
- `ps` 的 `wink` 显示**数据窗**大小；`stk/sz` 显示栈窗大小；不再有
  “栈 + guard 在同一个窗口里”的概念。

## 6. 平台差异

| 端口 | 行为 |
|---|---|
| C3（rv32 TOR） | 已启用；默认拒绝；栈块按 4 KiB 对齐，窗口外的栈段用 TOR 精确描述 |
| RP2350 Hazard3（rv32 NAPOT） | 已启用；栈块是自然对齐的 2 的幂，一个 NAPOT 条目即可；`stack_align` 不再约束窗口顶 |
| RP2350 M33（PMSAv8） | 已启用；栈块一个区域；保留 32 B landing（异常压栈失败时才有帧），guard 在块下；MSTKERR 现在也提 `declare TOS_STACK` |
| x86 / RP2040 | 未启用（`PLAT_STACK_AREA = 0`）：栈仍在数据窗，保留 `plat_user_stack_guard()`（canary + syscall 时的 `sp` 检查）。它只是检测：下一次 syscall 时发现 sp 在栈外或 canary 被踩，然后杀任务；**不能阻止**一次大帧直接跳过栈窗写进邻栈。所以这两个端口的栈隔离是**尽力而为**，不是保证 |

> x86/RP2040 的软件守卫与栈区方案并不冲突：即使栈不在堆里，分工仍是
> “canary + sp 检查发现漂移，硬件不提供的事不做承诺”。

## 7. 收益（C3）

| 程序 | 现在（span+栈+guard） | 之后（数据窗 + 栈窗） |
|---|---|---|
| `sh` | 24 | **20**（16 + 4） |
| `tools` | 20 | **16**（12 + 4） |
| `help`/`top`/`ls`/`sleep` | 12 | **8**（4 + 4） |
| `edit` | 24 | **20**（16 + 4） |
| `csc` | 108 | **104**（88 + 16） |

- 每个带栈的实体省 4 KiB；
- 每个任务少一条 PMP/MPU 区域（不再有 guard 区）；
- 堆与栈不再共享窗口空间，`TOS_HEAP` 的语义变简单；
- 构建期帧审计从安全网降级为友好提示（越界必然 fault）。

## 8. 代价与风险

- 每个进程从 1 个窗口变成 2 个：已实现为“数据窗 + 栈块”，`ps` 的 `wink`
  只显示数据窗；`free` 用 `SYS_STACKINFO` 单独列出栈区的 reserved/live/peak，
  不再需要从 `tasks` 行里猜；
- 栈区碎片与地址空间上限（`MAX_THREADS=32 × 4 KiB = 128 KiB`）：buddy 分配
  只允许 2 的幂的块，混合 4/8/16 KiB 请求也不会产生 first-fit 那种不可用碎片；
  上限仍由配置决定；
- **窗口放置策略**：实际实现选择了“`win_alloc` 的上界跟着栈区下沿走”
  （没有固定 headroom，也没有改成从高地址端向下放）。增长时扫描窗口表，
  只在新下沿仍高于最高数据窗 + 4 KiB 时扩；缩小后地址空间重新可被数据窗使用；
- ARM 异常诊断变化：RP2350 M33 已做。landing 从 0xF00 缩到 32 B（旧值在
  栈区下会写穿相邻栈），MSTKERR 和普通 guard fault 都提 `TOS_STACK` 并杀；
- x86/RP2040 需要两套策略，不能“删掉所有 guard”：还没做，`PLAT_STACK_AREA`
  仍为 0；
- 属于窗口模型的大改；C3 实机已跑，RP2040/Pico 2 的 T3 未接板，是 `NOT RUN`。

## 9. 验收

1. **已做（C3）**：主线程 + kthread 栈都进栈区、删 guard、只映射当前栈；
2. **已做（C3）**：`kthreadtest`/`prodcons`/`deep`/`accept` 全过；
3. **部分**：`ps` 的 `wink`/`stk/sz` 与本文第 7 节一致（`sh` 16+4，`stk` 4+16）；
4. **已做**：故意下溢立即 fault（不是下一次 syscall 的 canary），shell 继续；
5. **已做**：栈窗耗尽时 `thread_create` 干净地失败，进程表/窗口不泄漏；
6. **已做**：`SYS_STACKINFO`（`struct stackarea`）把 reserved/live/peak 交给
   用户态；`free` 多一行 `stack area`（没有栈区的板子不打印）。`kthreadtest`
   的 1000 次 4K/16K 混合 create/join 在 C3 上 1.3 s 跑完，`live` 和
   `reserved` 回到基线；没有栈区的板子打印 `N/A`。不 join 的线程随进程退出
   一并归还也已覆盖；
7. T1/T2 全过；RP2040/Pico 2 的 T3 是 `NOT RUN`（未接板），见验收卡片。

## 10. 明确不做

- 不提供跨线程栈指针（共享走 global/static）；
- 不为省一条 PMP 而做非 2 的幂栈大小；
- 不在 x86/RP2040 上假装有硬件边界。
