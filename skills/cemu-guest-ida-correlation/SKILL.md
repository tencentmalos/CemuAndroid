---
name: cemu-guest-ida-correlation
description: Combine a live Cemu Guest GDB Remote session with an IDA Pro database using exact RPX, database, and identity-manifest hashes; prove Guest EA to IDA EA mappings; compare live memory with static bytes; capture breakpoint registers and threads; and promote annotations by evidence level. Use for dynamic-plus-static Guest reverse engineering, IDA MCP correlation, validating BetterVR or graphic-pack addresses, or turning runtime PPC observations into auditable IDA symbols and comments.
---

# Cemu Guest 与 IDA 联合分析

把 Guest runtime 事实与 IDA 静态解释通过同一个 Guest effective address 和身份哈希绑定。
不允许用符号名相似度或手算偏移直接武装断点。

## 先读取

- 阅读 `../../docs/architecture/guest-reverse-debug-mod-pipeline.md` 第 3、4、7、8 节。
- 先用 `../cemu-guest-executable-ida/SKILL.md` 建立可信数据库。
- 再用 `../cemu-guest-runtime-debug/SKILL.md` 完成 warmup、连接和安全停点。
- 涉及 Mod 时同时使用 `../cemu-guest-game-patching/SKILL.md`。

## 1. 建立三重身份门禁

开始前计算并记录：

1. 当前运行 RPX SHA-256 与 module checksum；
2. 当前打开 `.i64` SHA-256；
3. `cemu.guest-executable.v1` identity manifest SHA-256。

同时要求 title ID/version、module name、entry point、text/data section 映射一致。任何一项
不符都停止；不要继续换算、断点或更新 IDA 注释。

通过 ida-pro-mcp 读取实际打开 database 的 input path、imagebase、processor、字节序和分析
状态。不要假设“当前 IDA 窗口”就是目标数据库。

## 2. 在精确停止点绑定 IDA

1. 在 `guest_debug` 会话暂停 Guest，保存当前 `stopEpoch`；
2. `list_modules` 获取目标 module 的 Guest image base；
3. 调用 `bind_ida_database`，传入 RPX、IDB、identity manifest 的路径与期望 SHA；
4. 记录 correlation ID、Guest/IDA image base 和 mapping mode；
5. 对目标地址以及 text 段内至少另外两个 anchor 调用 `correlate_addresses`；
6. 只接受返回的逐地址 proof。

通用公式：

```text
ida_ea = ida_image_base + (guest_address - guest_image_base)
```

若 loader 保留 Guest linked address，两个 base 可相同；仍必须保存 proof。若地址来自 Host JIT
PC，先通过 Cemu JIT block metadata 还原 Guest PC，不能直接输入公式。

## 3. 做字节级交叉验证

在同一 `stopEpoch`：

1. `guest_debug.read_memory` 读取候选地址附近 16–32 字节；
2. ida-pro-mcp 在 proof 的 IDA EA 读取相同范围；
3. 比较完整字节串与 PowerPC big-endian 反汇编；
4. 核对函数边界、被覆盖原指令和 continuation；
5. 字节不一致立即停止，视为版本、mapping 或数据库漂移。

内存读取结果是一级 runtime 事实；IDA 名称、反编译和类型是解释性事实，不得反向覆盖前者。

## 4. 用断点提升证据等级

1. 在当前 epoch 添加 session-owned software breakpoint；
2. 继续 Guest，并等待更大的 `stopEpoch`；
3. 要求 stop reason 为 breakpoint 且 PC 精确等于 proof 的 Guest EA；
4. 保存 LR、CTR、r1、r3-r10、Guest thread ID/name 和同地址 bytes；
5. 移除断点并恢复 Guest；
6. Guest 恢复后再进行耗时的 IDA 交叉引用、调用图、反编译和类型分析。

不要为了等待 IDA 无限续租。正常清理、EOF 或 lease 到期都必须恢复原指令和 Guest。

## 5. 回写可审计结论

使用以下证据前缀：

| 等级 | 可写入内容 |
| --- | --- |
| `reference` | BetterVR、其他平台仓库或文档提供的候选语义 |
| `static` | 当前 Wii U RPX 的字节、字符串、控制流和调用关系支持 |
| `runtime` | 当前身份下的断点、寄存器、线程、内存或 Guest counter 命中 |
| `semantic` | 参数/结构解释与最小 A/B Mod 行为一致 |

断点命中只把“该地址路径执行”提升到 `runtime`，不自动证明完整 C++ 原型或业务名。把
correlation ID、RPX/IDB/manifest SHA、stopEpoch、场景和证据路径写入 IDA comment、符号 CSV
或 `docs/architecture/`；更新 `.i64` 后重新计算 SHA、提交 private 子仓，再更新父仓 gitlink。

最终报告必须逐条给出：身份门禁、地址 proof、静态/动态 bytes、命中 PC/线程/寄存器、清理
结果、最高证据等级和仍需验证的假设。
