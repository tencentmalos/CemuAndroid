# Wii U 游戏数据源与查询方法

> 记录 2026-09-19 整理 [Top 100 榜单](README.md) 时实际用到的数据源、可复现命令和踩过的坑。
> 下次要更新榜单、查某款游戏的语言/封面/兼容性，或给其他主机做同类整理时，从这里开始。

## 快速索引

| 想查什么 | 用哪个源 | 是否需要代理 |
|:--|:--|:--|
| 游戏 ID、语言、封面、类型、发行商、ROM 校验和 | GameTDB Wii U 数据库 | **主站需要**，图片 CDN 不需要 |
| Cemu 兼容性评级、已知问题、graphic pack 说明 | Cemu 官方 Wiki API | 不需要 |
| 权威中文名、大众热度（浏览量） | Wikipedia / Wikimedia API | 不需要 |
| 官方销量 | en.wikipedia「List of best-selling Wii U video games」 | 不需要 |
| 民间汉化清单（含汉化组署名） | 社区「WIIU中文游戏全集」目录 | 不需要 |

---

## 1. GameTDB — Wii U 全库元数据

[GameTDB](https://www.gametdb.com) 是 Wii/Wii U/3DS/Switch 的社区元数据库，一次下载即可离线查询全部
**2860 条** Wii U 记录（含实体卡带、eShop 下载版、Virtual Console、系统应用）。

### 下载整库

```sh
curl -sL -o wiiutdb.zip "https://www.gametdb.com/wiiutdb.zip?LANG=ORIG" && unzip -o wiiutdb.zip
```

`LANG=ORIG` 表示保留各区原始标题。换成 `LANG=ZHCN` 会让 `<title>` 变成社区填写的中文译名，
但覆盖率极低（Wii U 仅 14 条，且都是 Wii VC 移植），**不要指望用它拿中文名**。

> **主站被墙**：`www.gametdb.com` 在本机直连超时（DNS 解析正常 → SNI 阻断）。
> 需要走代理，本仓库环境用 `~/.claude/skills/clash-isolated-proxy/start.sh`（隔离实例，
> 不改系统代理、不开 TUN）。图片 CDN `art.gametdb.com` **可以直连**，见下文。

### 单条记录的字段结构

```xml
<game name="The Legend of Zelda: Breath of the Wild (USA) (EN)">
  <id>ALZE01</id>                      <!-- 6 位游戏 ID，第 4 位是区域码 -->
  <type>WiiU</type>                    <!-- WiiU / VC / Channel ... -->
  <region>NTSC-U</region>              <!-- NTSC-U / PAL / NTSC-J -->
  <languages>EN</languages>            <!-- 游戏内置语言（关键字段） -->
  <locale lang="EN"><title>...</title></locale>
  <developer>Nintendo</developer>
  <publisher>Nintendo</publisher>
  <date year="2017" month="3" day="3" />
  <genre>action,adventure,role-playing</genre>
  <rating type="ESRB" value="E10+"><descriptor>...</descriptor></rating>
  <wi-fi players="0"><feature>download</feature></wi-fi>
  <input players="1"><control type="pad" required="false" /></input>
  <rom version="0" name="....wud" size="25025314816"
       crc="4294d948" md5="..." sha1="..." />
</game>
```

### 按 ID 前缀/区域筛选

ID 第 4 位是区域码，用来推导封面目录和判断发行区域：

| 第 4 位 | 区域 | 封面目录 |
|:--:|:--|:--|
| `E` | 北美 NTSC-U | `US` |
| `P` | 欧洲 PAL | `EN` |
| `J` | 日本 NTSC-J | `JA` |
| `K` | 韩国 | `KO` |
| `D`/`F`/`S`/`I`/`X`/`Y`/`Z` | 欧洲各语言变体 | `EN` |

### 离线查询示例

统计语言分布（本次得出的关键结论就靠它）：

```python
import xml.etree.ElementTree as ET
from collections import Counter

root = ET.parse('wiiutdb.xml').getroot()
c = Counter()
for g in root.findall('game'):
    for l in (g.findtext('languages') or '').split(','):
        if l.strip():
            c[l.strip()] += 1
print(c.most_common())
```

实测输出（2026-09-19，库版本 `20260919151115`）：

```
EN 2078, JA 819, FR 475, ES 450, DE 445, IT 395, NL 127, PT 97, RU 96,
DK 21, SE 18, NO 17, FI 14, TR 2, KO 1, ZHTW 1, ZHCN 1
```

**ZHTW/ZHCN 各只有 1 条，且都指向 YouTube 应用**（ID `HNKA`）。结合「全库无任何台/韩区发行记录」
（`region` 只有 NTSC-U/PAL/NTSC-J）以及 Wii U 从未在中港台正式发售且锁区这一事实，可以下一个硬结论：
**没有任何一款 Wii U 游戏原生内置中文**。榜单里的「中文」要么是欧美版自带的简繁中文（极少数第三方作品），
要么来自民间汉化。

### 封面图

图片 CDN 是 `art.gametdb.com`，**可以直连**（只有主站被墙）：

```
https://art.gametdb.com/wiiu/<style>/<REGION_DIR>/<GAME_ID>.<ext>
```

| style | 扩展名 | 说明 |
|:--|:--|:--|
| `cover` | `.jpg` | 正面封面 176×248，体积小，适合表格缩略图 |
| `coverHQ` | `.jpg` | 高清正面封面 |
| `cover3D` | `.png` | 带立体盒体透视 |
| `coverfull` / `coverfullHQ` | `.jpg` | 完整展开封面（含侧脊+封底） |
| `disc` / `discM` | `.png` | 光盘盘面 |
| `backHQ` / `backM` | `.jpg` | 封底 |

```sh
curl -sL -o cover.jpg "https://art.gametdb.com/wiiu/cover/US/ALZE01.jpg"
```

**两个坑**：

1. **扩展名不统一**。`cover`/`coverHQ`/`coverfull`/`back` 是 `.jpg`，`cover3D`/`disc` 是 `.png`。
   用错扩展名一律 404。
2. **Python `urllib` 对这个 CDN 会超时**，`curl` 却稳定 200。本次改用 `subprocess` 调 `curl` 解决，
   见 [`fetch_covers.py`](scripts/fetch_covers.py)。原因未深究，推测与默认请求头有关。

同一游戏多区域发行时，应按 ID 列表依次尝试（`ALZE01` → `ALZP01` → `ALZJ01`），并对每个 ID
按其区域码优先的目录顺序回退。少数游戏（如日本线上专属 `Monster Hunter: Frontier G` 的 `AMFJ`）
确实没有封面，只能退到同系列实体版。

---

## 2. Cemu 官方 Wiki — 兼容性评级

[wiki.cemu.info](https://wiki.cemu.info) 是标准 MediaWiki，**可以直连**，提供开放 API。
它是判断"这游戏能不能在 Cemu 上玩"的唯一权威源。

### 拉取全部游戏页

```sh
# 1) 列出主命名空间全部页面（分页，aplimit 上限 500）
curl -s "https://wiki.cemu.info/api.php?action=query&list=allpages\
&apnamespace=0&aplimit=500&format=json&formatversion=2"

# 2) 批量取 wikitext（一次最多 40~50 个标题，用 | 分隔）
curl -s "https://wiki.cemu.info/api.php?action=query&prop=revisions\
&rvprop=content&rvslots=main&titles=Mario%20Kart%208|Splatoon\
&format=json&formatversion=2"
```

2026-09-19 实测：主命名空间 1924 页，其中 **1215 页**含 `{{Infobox VG}}`（即游戏页）。

### Infobox 字段

```wikitext
{{Infobox VG
|title = The Legend of Zelda: Breath of the Wild
|image = [[File:The_Legend_of_Zelda_Breath_of_the_Wild.png|295px]]
|developer = Nintendo EPD
|publisher = Nintendo
|series = The Legend of Zelda
|released = {{vgrelease|WW= March 3, 2017}}
|genre = Action-adventure
|modes = Single-player
|input = Amiibo, GamePad, Pro Controller
|wikipedia = The Legend of Zelda: Breath of the Wild
|titleid = 00050000-101C9300, 00050000-101C9400, 00050000-101C9500
|rating = Perfect
}}
```

`rating` 是五档兼容性评级，也是本榜单「Cemu 兼容性」一列的唯一来源：

| 评级 | 含义 |
|:--|:--|
| `Perfect` | 可全程通关，无已知影响体验的问题 |
| `Playable` | 可通关，有轻微图形/音频瑕疵 |
| `Runs` | 能进游戏，但有明显问题或需额外设置 |
| `Loads` | 能启动到标题/菜单，无法正常游玩 |
| `Unplayable` | 崩溃或黑屏 |

### 解析 Infobox 的两个坑

1. **必须做花括号配对**，不能用固定字符长度截断。`released` 里嵌套的 `{{vgrelease|...}}`
   会让"取前 2500 字符"的做法在 Virtual Console 页上失败——那些页面的 `rating` 排在很后面，
   截断后正则匹配到的是 `}}`。本次首轮解析 425 条拿到 `'}}'` 就是这个原因。
2. **要剔除 Virtual Console 页**。1215 个游戏页里有 **437 个**是 VC 复刻（`|type = Virtual Console`），
   它们不是原生 Wii U 作品，且多数没填 `rating`。剔除后剩 **582 款**原生 Wii U 游戏。

```python
def infobox(txt):
    """花括号配对提取完整 Infobox 块。"""
    i = txt.find('{{Infobox VG')
    if i < 0:
        return ''
    depth, j = 0, i
    while j < len(txt) - 1:
        if txt[j:j+2] == '{{':
            depth += 1; j += 2; continue
        if txt[j:j+2] == '}}':
            depth -= 1; j += 2
            if depth == 0:
                return txt[i:j]
            continue
        j += 1
    return txt[i:]
```

### wiki 页面信息量 = 模拟器社区关注度

一个意外好用的指标：**页面 wikitext 字节数**能很好地代表 Cemu 社区对该游戏的关注程度。
页面越长意味着社区记录的已知问题、graphic pack 配置、性能建议越多。

实测（字节）：BotW 82750、马里奥赛车8 69228、大乱斗 50062、3D世界 45081，
而冷门作普遍只有 1000~2000。

相比 Wikipedia 浏览量，它的优势是**天然排除"多平台大作在别处很火但没人用 Cemu 玩"的偏差**——
《使命召唤：黑色行动2》的 Wikipedia 浏览量（39.6 万）远超 BotW 以外的所有作品，但它的
Cemu wiki 页只有 5955 字节。榜单因此把这个指标的权重设为最高（60%）。

---

## 3. Wikipedia / Wikimedia — 中文名与热度

均**可以直连**，无需代理。

### 权威中文名（跨语言链接）

```sh
curl -s "https://en.wikipedia.org/w/api.php?action=query&prop=langlinks\
&lllang=zh&redirects=1&titles=Mario%20Kart%208&format=json&formatversion=2"
```

一次最多 20 个标题（带 `redirects=1` 时）。返回 `langlinks[0].title` 即中文条目名。

**注意过滤**：结果可能指向「XX系列」或「Wii U遊戲列表」这类聚合页，不是该作的专有译名，必须剔除。
本次的处理是丢弃含「系列」「列表」的结果，再单独补。

### 近 12 个月浏览量（热度）

```sh
curl -s -H "User-Agent: your-tool/1.0" \
  "https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/\
en.wikipedia/all-access/user/The_Legend_of_Zelda:_Breath_of_the_Wild/monthly/2025090100/2026083100"
```

- 路径里的标题要 URL 编码，空格换成 `_`
- `user` 过滤掉爬虫流量，比 `all-agents` 更可信
- 必须带 `User-Agent`，否则可能被限流
- 请求间隔建议 ≥0.35s；密集请求会撞 **HTTP 429**

### 官方销量

en.wikipedia 的 [List of best-selling Wii U video games](https://en.wikipedia.org/wiki/List_of_best-selling_Wii_U_video_games)
只有 **20 款**破百万（Wii U 总销量 1356 万台）。做加权时要注意：只有 20 个样本有值，
若把"无数据"当 0 分会在榜单中部造成人为断层。本次的处理是有数据者落在 0.55~1.0 区间，
无数据者统一取 0.5（视为"未达百万但不额外惩罚"）。

### 用游戏名反查 Wikipedia 条目的坑

直接用游戏名批量匹配，会匹配到**同名的通用概念页**，浏览量严重虚高：

| 游戏 | 误匹配到 | 该页浏览量 |
|:--|:--|--:|
| `IQ Test` | Intelligence quotient（智商） | 548,988 |
| `Red Riding Hood` | Little Red Riding Hood（小红帽） | 503,085 |
| `Teslapunk` | Steampunk（蒸汽朋克） | 338,527 |
| `Hello Kitty Kruisers` | Hello Kitty（品牌页） | 745,559 |
| `Amazon Instant Video` | Amazon Prime Video（流媒体服务） | 908,819 |
| `Disney Planes` | Planes (film)（电影） | 265,715 |

**对策**：搜索时加 `video game` 限定，并校验返回标题与游戏名的相似度（去掉消歧义括号后
比较归一化字符串，要求相等或互为子串）。仍无法自动判定的列入人工黑名单，浏览量按 0 计。

另一类是匹配到**系列页或原版页**（`Twilight Princess HD` → `Twilight Princess`、
`Batman: Arkham City Armored Edition` → `Batman: Arkham City`）。这类主体相关，浏览量偏高但可接受，
本次予以保留。

---

## 4. 中文支持（民间汉化）

这是最容易写错的一列，**不要靠印象填**。

### 可用的社区目录

社区整理的「WIIU中文游戏全集」共 **133 条**，每条带区域、汉化类型和汉化组署名，
去重后是 **62 个独立标题**（133 条里含 WUP 直装版和多区域重复）：

- <https://www.oldmanemu.org/家机游戏/wiiu/wiiu中文游戏全集>
- <https://bbs.xqemu.cn/thread-3727-1-1.html>

条目格式：

```
WIIUCH051 – 塞尔达传说 荒野之息[日版][简繁体汉化][v208][91WII汉化组]
WIIUCH065 – 异度之刃X[日版][简体汉化][游侠LMAO汉化组]
WIIUCH015 – 超级食肉男孩[欧版][官方繁中]
```

据此可以区分两类，本榜单分别标为「✅ 官方」和「🈶 汉化」：

- **官方中文**：欧美版自带简/繁中文，多为第三方独立游戏
  （暗黑血统战神版、超级食肉男孩、我的世界、迷失之地、铲子骑士欧版等）
- **民间汉化**：91WII、游侠LMAO、猫星、ACG、扑家、咸鱼、漫游、无名、Advance 等汉化组制作

### 主要汉化组分工（观察）

| 汉化组 | 代表作 |
|:--|:--|
| 91WII汉化组 | 产量最高，覆盖塞尔达风之杖/荒野之息、马里奥赛车8、喷射战士、超级马里奥制造、幻影异闻录等 |
| 游侠LMAO汉化组 | 异度之刃X、猎天使魔女2 |
| 猫星汉化组 | 皮克敏3、蘑菇队长、星之卡比彩虹诅咒 |
| ACG汉化组 | 塞尔达黄昏公主HD、零濡鸦之巫女、怪物猎人3G HD（合作） |
| 扑家汉化组 | 生化危机启示录HD、魔窟冒险 |
| 咸鱼汉化组 | 超级马里奥3D世界 |
| 米拉建工&星组&XXGAME | 异度之刃X 最终版（多字体版本） |

### 抓不到的源

- **romhacking.net**：有正式的翻译补丁数据库（`?platform=52&language=6` 是 Wii U + Chinese），
  但对脚本和 WebFetch 都返回 **403**，实测是人机验证拦截。未纳入本次交叉核对。
- **kancloud.cn 的中文游戏列表**：WebFetch 取到空内容。

因此「中文」一列的口径是：**以上述社区目录为准，目录未收录 ≠ 一定没有汉化**。

---

## 5. 本次用到的脚本

都在 [`scripts/`](scripts/) 下，直连可跑（除 GameTDB 整库下载需代理）：

| 脚本 | 作用 |
|:--|:--|
| [`fetch_direct.py`](scripts/fetch_direct.py) | Wikimedia 浏览量抓取（显式禁用代理 + 退避重试） |
| [`fetch_wiki_meta.py`](scripts/fetch_wiki_meta.py) | en.wikipedia 条目解析、langlinks、批量匹配 |
| [`fetch_pv2.py`](scripts/fetch_pv2.py) | 带相似度校验的条目反查（避免匹配到概念页） |
| [`fetch_covers.py`](scripts/fetch_covers.py) | GameTDB 封面下载（curl 实现 + 区域目录回退） |
| [`build_final.py`](scripts/build_final.py) | 合成榜单：秩次融合热度、合并中文与元数据 |
| [`gen_md.py`](scripts/gen_md.py) | 生成 [README.md](README.md) |
| [`zh_map.py`](scripts/zh_map.py) | 汉化目录中文名 → Cemu Wiki 英文名映射表 |

### 复现完整流程

```sh
# 0) GameTDB 整库（需代理）
~/.claude/skills/clash-isolated-proxy/start.sh
curl -x http://127.0.0.1:7899 -sL -o wiiutdb.zip \
  "https://www.gametdb.com/wiiutdb.zip?LANG=ORIG" && unzip -o wiiutdb.zip
~/.claude/skills/clash-isolated-proxy/stop.sh      # 用完必须关

# 1~4) 以下均直连
#   抓 Cemu wiki 全部游戏页 → 解析 Infobox → 补 Wikipedia → 抓浏览量
#   （见各脚本；注意 Wikimedia 请求间隔 ≥0.35s，否则 429）

# 5) 合成 + 生成文档
python3 build_final.py && python3 gen_md.py
```

---

## 6. 给其他主机做同类整理时

- **3DS**：GameTDB 有 `3dstdb.zip`，字段结构一致；模拟器兼容性看 Citra/Azahar 的兼容性列表
- **Switch**：GameTDB 有 `switchtdb.zip`；兼容性看 Ryujinx/yuzu 系的兼容性数据
- **Wii**：GameTDB 有 `wiitdb.zip`；兼容性看 Dolphin wiki（结构与 Cemu wiki 类似，也是 MediaWiki）

三条通用经验：

1. **不要以 Wikipedia 命中作为入榜前提**。应以模拟器自己的兼容性 wiki 为主库，Wikipedia 只作增强。
   本次初版就因为这个疏漏，漏掉了《异度之刃X》《耀西的毛线世界》《幻影异闻录♯FE》《神奇101》
   《僵尸U》《乐高城市卧底》等 15 款重要作品——它们的 Cemu wiki 页很长（异度之刃X 有 36026 字节，
   社区关注度排第 3），只是 Wikipedia 批量匹配失败。
2. **热度用秩次融合，不要直接加权原始数值**。浏览量、销量、页面长度量纲差异巨大，
   直接相加会被某一维度主导；用百分位秩能保持各维度的相对贡献。
3. **中文支持必须实证**。任何主机都要先搞清楚"有没有官方中文"这个前提
   （查发行区域 + 库内语言字段），再区分官方与民间汉化，不要把两者混为一谈。
