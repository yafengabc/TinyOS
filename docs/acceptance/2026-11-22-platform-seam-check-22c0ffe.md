# platform 接缝检查修正（#52 review 后续，2026-11-22）

```text
commit:            22c0ffe（include 规则按名字池 + 扫头文件；plat_* 全量钉住）
scope:             internal/build/include_test.go、testdata/platform-periph.txt
T0:                PASS — go test -count=1 ./...
T1/T2/T3/T4:       NOT RUN（只改测试与基线，不动构建输入）
```

## 改了什么

1. **两条空转规则**：原来匹配 `#include "board/..."` / `"drivers/..."`，全仓 0 命中
   （构建把目录放在 `-I` 里，代码写裸名字）。改为名字池：`board/**/*.h` 的 basename
   减去更近目录（kernel/include）遮蔽的同名头；`drivers/**/*.h` 同理减去
   kernel/arch/include。
2. **扫描 `.h`**：驱动头文件里 include 内核私有头同样会泄漏给所有包含它的 `.c`。
3. **`plat_*` 声明全量钉住**：`platform-periph.txt` 从 26 条多词声明扩到 28 条
   （含 `plat_init`、`plat_pio` 这类单段名），正则放宽到任意返回类型和
   `static inline`；并且**双向**校验：新增声明红，钉住但已消失也红。
4. 汇总日志区分"25 条基线 / 17 个文件 / 28 个钉住声明"。

## 负例（新增单测 + 手工各一次）

```text
kernel/console.c + #include "rp2350rv_memmap.h"
  -> kernel/console.c:438: kernel must not include board-only headers
drivers/usb/rp2350rv/usb_cdc.h + #include "heap.h"
  -> drivers/usb/rp2350rv/usb_cdc.h:49: drivers must not include kernel-private headers
kernel/platform.h + int plat_lcd_probe(void);
  -> new plat_* declaration plat_lcd_probe (move it to kernel/plat/)
```

`TestIncludeRulesDetect` / `TestPlatformDeclsDetect` 把这些固定成 T0 用例，包括
"被更近目录遮蔽的同名头不算违规"和"注释里的 `plat_x()` 不算声明"。

## 没做

- 驱动改用 `plat/*.h` 的 include 迁移（阶段 0 外）。
- 构建路径不可复现（`__FILE__` 绝对路径）：#52 卡片里"4 B 差异"的来源，单独一项。
