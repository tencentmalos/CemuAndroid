"""直连（不经任何代理）抓取 Wikipedia 元数据与浏览量。"""
import json, urllib.request, urllib.parse, time

# 显式禁用代理，避免继承环境变量
_op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
_op.addheaders = [('User-Agent', 'wiiu-top100-research/1.0 (local offline analysis)')]

def get(url, tries=4, pause=0.35):
    for a in range(tries):
        try:
            time.sleep(pause)
            with _op.open(url, timeout=45) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 404: return None
            time.sleep(3 + 3*a)
        except Exception:
            time.sleep(2 + 2*a)
    return None

def pageviews(wp_title, start="2025090100", end="2026083100"):
    u = ("https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/"
         "en.wikipedia/all-access/user/"
         + urllib.parse.quote(wp_title.replace(' ', '_'), safe='')
         + f"/monthly/{start}/{end}")
    d = get(u)
    if not d: return None
    items = d.get('items', [])
    if not items: return None
    return {'total': sum(x['views'] for x in items), 'months': len(items)}
