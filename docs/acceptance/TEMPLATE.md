# <变更名>：<目标/板>（<日期>）

```text
commit:            <短 sha>（被测提交）
change:            <一句话：改了什么>
baseline:          <短 sha>（重构/修复前；没有就写 none）
target/config:     <target> / <config file 或 defaults>
board/port/image:  <板子 + 端口 + 镜像名；纯 QEMU 写 target 即可>
toolchain:         go <ver>, clang <ver>, qemu <ver>, python <ver>
host:              <OS / 机器>
T0:
T1:
T2:
T3:
T4:
logs:              <本地路径；或 "not kept"（原始日志不入库）>
notes:
```

## 执行的命令与结果

```sh
# 逐条列出，带结果
```

## 关键输出

```text
（≤30 行；只留能证明结论的那几行）
```

## 没跑的

- 哪些项 `NOT RUN`，为什么（链接 `docs/OPEN-ITEMS.md` 的条目）。
