# BotW Wii U v208 逆向资产

这里保存可直接复用的 BotW Wii U v208 静态逆向资产。目录按游戏、平台和 Guest 版本组织，
所有地址、符号和运行时证据都绑定到同一份 RPX 身份，不能套用到其他区域、更新版本或
Switch 版 `references/botw`。

## 身份基线

| 项 | 值 |
| --- | --- |
| 游戏 | The Legend of Zelda: Breath of the Wild |
| 区域 | JP |
| title ID | `00050000101C9300` |
| update | `v208` |
| DLC | `v80`（运行环境；不参与主 RPX 哈希） |
| module | `u-king` |
| `moduleMatches` | `0x6267BFD0` |
| RPX SHA-256 | `ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30` |
| Guest entry | `0x03C70404` |
| 地址模型 | 32-bit big-endian PowerPC Guest effective address |

完整 section 映射见
[`identity/guest-executable.json`](identity/guest-executable.json)，建库工具与运行时证据见
[`identity/analysis-manifest.json`](identity/analysis-manifest.json)。原始 RPX 不提交；需要重建时
必须从当前运行中的、自有合法游戏内容重新导出并复核 SHA-256。

## IDA 数据库

数据库位于 private 子模块
[`ida-database/botw-jp-v208-u-king-ba58da5b95ce.i64`](ida-database/botw-jp-v208-u-king-ba58da5b95ce.i64)，
由子仓 Git LFS 管理。具备 private 仓库权限时执行：

```sh
git submodule update --init games/reverse/botw/wiiu-v208/ida-database
git -C games/reverse/botw/wiiu-v208/ida-database lfs pull
```

该 `.i64` 已经是完整数据库，直接用 IDA Professional 9.1 打开即可，不要求安装 RPL loader。
当前数据库包含：

- 8 个有效 segment，`.text` 为 `0x02000020..0x04347C2C`；
- 109,352 个已识别函数和 55,991 个字符串；
- 20 个以 `ref_bettervr_v208__` 开头的参考函数名；
- 40 个 profiler hook/continuation 注释；
- 18 个经过 AYANEO Pocket DS gameplay warmup 实际命中的 `runtime` 注释。

`ref_bettervr_v208__` 明确表示名称来源仍包含 BetterVR 参考语义；运行时命中只证明 hook 所在
路径执行，不能自动证明完整函数原型、类型和业务命名。section 34、38 在本次神庙场景没有
命中，仍保持 `reference` 等级。

## 动态调试校准

2026-08-01 在 AYANEO Pocket DS 的完整 gameplay warmup 后，`spatial_debug_tool` 通过 Cemu
PowerPC GDB Remote 连接到 `u-king`。IDA 和 Guest 在 `0x02C57E50` 读取到相同的 16 字节：

```text
900100049421ffe88004000080630008
```

该位置属于 `ref_bettervr_v208__uking_frm_System_preCalc + 0x4`。软件断点继续后约 159 ms
命中，PC 为 `0x02C57E50`，线程为 `0e192e80` / `Default Core 1`。`0x02C57E50`、
`0x03A146E4`、`0x037A5DB0` 的 Guest EA 到 IDA EA 映射均通过显式基址 proof，不能把这组
identity 映射外推到其他区域或版本。完整寄存器、线程分类、MCP stop lease 和清理规则见
架构文档第 8 节。

## 目录内容

```text
wiiu-v208/
├── README.md
├── ida-database/                        # private git submodule
│   └── botw-jp-v208-u-king-ba58da5b95ce.i64
├── identity/
│   ├── analysis-manifest.json
│   └── guest-executable.json
└── symbols/
    └── botw-v208-profiler-symbols.csv
```

端到端提取、RPL loader 构建、IDA 标定、GDB Remote、Mod 制作和证据升级规则统一见
[`docs/architecture/guest-reverse-debug-mod-pipeline.md`](../../../../docs/architecture/guest-reverse-debug-mod-pipeline.md)
与 [`skills/cemu-guest-game-patching/SKILL.md`](../../../../skills/cemu-guest-game-patching/SKILL.md)。
