"""Wii U Top 100 最终合成。

主库 = Cemu wiki 原生 Wii U 游戏页（有效兼容性评级），不以 Wikipedia 命中为入榜前提。
热度指数 = 三来源百分位秩加权：
  A 60%  Cemu wiki 页面信息量  —— 模拟器社区关注度（Cemu 用户视角，本榜核心）
  B 18%  官方累计销量          —— 当年市场表现（仅 20 款破百万；权重低于关注度，
         因 Wii U 装机量仅 1356 万，销量更多反映平台规模而非作品地位）
  C 22%  en.wikipedia 浏览量   —— 大众知名度（误匹配条目按 0 计，不倒扣）
"""
import json, re

def norm(s):
    s=s.lower(); s=re.sub(r'\(.*?\)','',s)
    return ' '.join(re.sub(r'[^a-z0-9]+',' ',s).split())

wiki = json.load(open('wiki_records.json'))
wtxt = json.load(open('wiki_game_wikitext.json'))
gtdb = json.load(open('gametdb_index.json'))
pv1  = json.load(open('wp_with_pv.json'))
pv2  = json.load(open('pv_extra.json'))
zhfix= json.load(open('zh_manual_verified.json'))
zhver= json.load(open('zh_verified.json'))

SALES = {
 'Mario Kart 8':8.46,'Super Mario 3D World':5.89,'New Super Mario Bros. U':5.82,
 'Super Smash Bros. for Wii U':5.38,'Nintendo Land':5.21,'Splatoon':4.95,
 'Super Mario Maker':4.02,'New Super Luigi U':3.07,
 'The Legend of Zelda: The Wind Waker HD':2.37,'Mario Party 10':2.27,
 'Donkey Kong Country: Tropical Freeze':2.02,'Wii Party U':1.79,
 'The Legend of Zelda: Breath of the Wild':1.70,"Yoshi's Woolly World":1.57,
 'Captain Toad: Treasure Tracker':1.37,'Pikmin 3':1.28,
 'The Legend of Zelda: Twilight Princess HD':1.17,'Lego City Undercover':1.15,
 'LEGO City: Undercover':1.15,'Hyrule Warriors':1.0,'Pokkén Tournament':1.0,
}
# Wikipedia 条目匹配到通用概念/品牌/影视页，浏览量不代表该游戏 -> 置 0
PV_INVALID = {
 'Amazon Instant Video','Hulu Plus','Netflix','YouTube','IQ Test','Red Riding Hood',
 'Teslapunk','Hello Kitty Kruisers','Disney Planes','Disney Planes: Fire & Rescue',
 'Monster High: New Ghoul in School','The Croods: Prehistoric Party!','Vaccine',
 'Paparazzi','Abyss','Sportsball','Tank! Tank! Tank!','Ghost Blade HD',
 'Minecraft: Wii U Edition','Wii Fit U','Wii Party U','Wii Sports Club',
 'Terraria','Super Meat Boy','The Binding of Isaac: Rebirth','Jewel Quest',
 'Star Sky','Little Inferno','Adventures of Pip','Cube Life Island Survival',
}
NOT_GAME = {
 'Wii U Menu','YouTube','Netflix','Hulu Plus','Amazon Instant Video','Nintendo TVii',
 'Internet Browser','Download Management','Friend List','Nintendo eShop','Miiverse',
 'Wii U Chat','Mii Maker','Wara Wara Plaza','Health and Safety Information','Daily Log',
 'Amazon Video','Wii Karaoke U','Crunchyroll','Wii Street U','BAKPTL',
}
RATING_CN   = {'Perfect':'完美','Playable':'可玩','Runs':'可运行','Loads':'仅加载','Unplayable':'无法运行'}
RATING_RANK = {'Perfect':5,'Playable':4,'Runs':3,'Loads':2,'Unplayable':1}

def zh_clean(z):
    """剔除指向系列页/列表页的模糊中文名。"""
    if not z: return None
    if any(b in z for b in ('系列','列表')): return None
    return z

rows=[]
for name, w in wiki.items():
    if name in NOT_GAME: continue
    if 'virtual' in (w.get('type') or '').lower(): continue
    rating=(w.get('rating') or '').strip().capitalize()
    if rating not in RATING_CN: continue

    m = pv1.get(name) or pv2.get(name) or {}
    wp_title = m.get('wp_title')
    views = 0
    if m.get('pv') and name not in PV_INVALID:
        views = m['pv']['total']

    zh = zh_clean(zhfix.get(name)) or zh_clean((zhver.get(name) or {}).get('zh')) or zh_clean(m.get('zh'))
    g  = gtdb.get(norm(name), {})

    rows.append({
        'name':name,'zh':zh,'wp_title':wp_title,
        'depth':len(wtxt.get(name,'')),'sales':SALES.get(name,0.0),'views':views,
        'rating':rating,'rating_cn':RATING_CN[rating],'rating_rank':RATING_RANK[rating],
        'ids':g.get('ids',[])[:4],'langs':g.get('langs',[]),'regions':g.get('regions',[]),
        'genre':(g.get('genre') or w.get('genre') or ''),
        'publisher':g.get('publisher') or w.get('publisher') or '',
        'developer':g.get('developer') or w.get('developer') or '',
        'year':g.get('year'),'titleid':w.get('titleid',''),'series':w.get('series',''),
    })

def pct_rank(vals):
    n=len(vals)
    if n<2: return [1.0]*n
    order=sorted(range(n), key=lambda i: vals[i])
    pr=[0.0]*n; i=0
    while i<n:
        j=i
        while j+1<n and vals[order[j+1]]==vals[order[i]]: j+=1
        avg=(i+j)/2.0
        for k in range(i,j+1): pr[order[k]]=avg/(n-1)
        i=j+1
    return pr

rA=pct_rank([r['depth'] for r in rows])
rC=pct_rank([r['views'] for r in rows])

# 销量维度：仅 20 款有公开百万销量数据。若直接把"无数据"当 0 分，
# 会在第 19/20 名之间制造人为断层。改为：有数据者在 0.55~1.0 区间按销量排序，
# 无数据者统一取 0.5（视为"未达百万，但不额外惩罚"）。
sold=[r for r in rows if r['sales']>0]
sr={r['name']:p for r,p in zip(sold, pct_rank([r['sales'] for r in sold]))}
for r,a,c in zip(rows,rA,rC):
    b = 0.55+0.45*sr[r['name']] if r['sales']>0 else 0.5
    r['heat_raw']=60*a+18*b+22*c

rows.sort(key=lambda r:-r['heat_raw'])
top=rows[:100]
hi,lo=top[0]['heat_raw'],top[-1]['heat_raw']
for i,r in enumerate(top,1):
    r['rank']=i
    r['heat']=round(10+90*(r['heat_raw']-lo)/(hi-lo),1) if hi>lo else 100.0

json.dump(top, open('top100.json','w'), ensure_ascii=False, indent=1)
json.dump(rows, open('pool_all.json','w'), ensure_ascii=False)
print(f"pool={len(rows)} -> top100  (zh: {sum(1 for r in top if r['zh'])}/100, covers pending)")
print(f"{'#':>3} {'heat':>5} {'depth':>6} {'sal':>4} {'views':>8} {'compat':<10} name")
for r in top[:35]:
    print(f"{r['rank']:>3} {r['heat']:>5} {r['depth']:>6} {r['sales'] or '-':>4} {r['views']:>8,} {r['rating']:<10} {r['name'][:42]}")

# ============ 中文支持（实证数据）============
# 来源：oldmanemu.org / bbs.xqemu.cn「WIIU中文游戏全集」133 条目录，去重后 62 个标题。
# 官方中文极少（Wii U 从未在中港台正式发售且锁区，GameTDB 全库 2860 条仅 YouTube 应用带中文标记），
# 因此绝大多数中文来自 91WII、游侠LMAO、猫星、ACG、扑家等民间汉化组。
import subprocess as _sp
zh_cat = json.load(open('zh_catalog.json'))
for r in top:
    c = zh_cat.get(r['name'])
    if c:
        kinds = '/'.join(c['kinds'])
        r['zh_support']   = '官方中文' if '官方' in kinds else '民间汉化'
        r['zh_kind']      = kinds
        r['zh_team']      = '、'.join(c['teams']) if c['teams'] else ''
        r['zh_cat_name']  = c['zh_name']
        r['zh_regions']   = c['regions']
    else:
        r['zh_support']='未见中文'; r['zh_kind']=''; r['zh_team']=''
        r['zh_cat_name']=''; r['zh_regions']=[]
    # 汉化目录里的中文名比 Wikipedia langlink 更贴近玩家习惯，优先采用
    if c and not r.get('zh'):
        r['zh'] = c['zh_name']

json.dump(top, open('top100.json','w'), ensure_ascii=False, indent=1)
n_off=sum(1 for r in top if r['zh_support']=='官方中文')
n_fan=sum(1 for r in top if r['zh_support']=='民间汉化')
print(f"\n中文支持: 官方 {n_off} / 民间汉化 {n_fan} / 未见 {100-n_off-n_fan}")
print(f"中文名覆盖: {sum(1 for r in top if r['zh'])}/100")
