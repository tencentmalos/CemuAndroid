"""为 Cemu-wiki 主库中 Wikipedia 未命中的条目补抓：条目名 + 中文名 + 浏览量。
搜索加 "video game" 限定并校验标题相似度，避免匹配到同名概念页。"""
import json, urllib.request, urllib.parse, time, re

_op=urllib.request.build_opener(urllib.request.ProxyHandler({}))
_op.addheaders=[('User-Agent','wiiu-top100-research/1.0')]

def get(url, tries=4):
    for a in range(tries):
        try:
            time.sleep(0.5)
            with _op.open(url, timeout=45) as r: return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code==404: return None
            time.sleep(3+3*a)
        except Exception: time.sleep(2+2*a)
    return None

def api(host, params):
    params.update({'format':'json','formatversion':'2'})
    return get(f"https://{host}/w/api.php?"+urllib.parse.urlencode(params))

def key(s): return re.sub(r'[^a-z0-9]','', re.sub(r'\(.*?\)','',s).lower())

def similar(a,b):
    ka,kb=key(a),key(b)
    return ka==kb or ka in kb or kb in ka

def resolve(name):
    """搜索 en.wikipedia，仅接受与游戏名足够相似的条目。"""
    d=api('en.wikipedia.org',{'action':'query','list':'search',
          'srsearch':f'{name} video game','srlimit':4,'srnamespace':0})
    if not d: return None
    for h in d.get('query',{}).get('search',[]):
        if similar(name, h['title']):
            return h['title']
    return None

def zh_of(title):
    d=api('en.wikipedia.org',{'action':'query','prop':'langlinks','lllang':'zh',
          'titles':title,'redirects':1})
    if not d: return None
    pg=d.get('query',{}).get('pages',[])
    if not pg: return None
    ll=pg[0].get('langlinks') or []
    return ll[0]['title'] if ll else None

def pageviews(t, start="2025090100", end="2026083100"):
    u=("https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/en.wikipedia/"
       "all-access/user/"+urllib.parse.quote(t.replace(' ','_'), safe='')+f"/monthly/{start}/{end}")
    d=get(u)
    if not d: return None
    it=d.get('items',[])
    return {'total':sum(x['views'] for x in it),'months':len(it)} if it else None

if __name__=='__main__':
    need=json.load(open('pv_need2.json'))
    out={}
    for i,n in enumerate(need):
        t=resolve(n)
        if not t:
            out[n]={'wp_title':None,'zh':None,'pv':None}
        else:
            out[n]={'wp_title':t,'zh':zh_of(t),'pv':pageviews(t)}
        if i%10==0:
            print(f"{i}/{len(need)} resolved={sum(1 for v in out.values() if v['wp_title'])}", flush=True)
            json.dump(out, open('pv_extra.partial.json','w'), ensure_ascii=False)
    json.dump(out, open('pv_extra.json','w'), ensure_ascii=False, indent=1)
    ok=sum(1 for v in out.values() if v['pv'])
    print(f"DONE resolved_pv={ok}/{len(need)}")
