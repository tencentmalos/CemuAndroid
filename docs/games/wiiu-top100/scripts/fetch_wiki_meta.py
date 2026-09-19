import json, urllib.request, urllib.parse, time, sys, re

PROXY="http://127.0.0.1:7899"
op=urllib.request.build_opener(urllib.request.ProxyHandler({'http':PROXY,'https':PROXY}))
op.addheaders=[('User-Agent','wiiu-top100-research/1.0 (local analysis)')]

def get(url, tries=4):
    for a in range(tries):
        try:
            with op.open(url, timeout=50) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code in (404,): return None
            if a==tries-1: return None
            time.sleep(2+a)
        except Exception:
            if a==tries-1: return None
            time.sleep(2+a)
    return None

def wp_search(title):
    """Resolve a game name to an en.wikipedia canonical article title."""
    u=("https://en.wikipedia.org/w/api.php?action=query&list=search&srsearch="
       + urllib.parse.quote(title + " Wii U")
       + "&srlimit=3&srnamespace=0&format=json&formatversion=2")
    d=get(u)
    if not d: return None
    hits=d.get('query',{}).get('search',[])
    return hits[0]['title'] if hits else None

def wp_info(titles):
    """Batch: resolve redirects, get zh langlink, get pageid."""
    out={}
    B=20
    for i in range(0,len(titles),B):
        chunk=titles[i:i+B]
        u=("https://en.wikipedia.org/w/api.php?action=query&prop=langlinks&lllang=zh&lllimit=500"
           "&redirects=1&titles="+urllib.parse.quote('|'.join(chunk))+"&format=json&formatversion=2")
        d=get(u)
        if not d: continue
        norm={n['from']:n['to'] for n in d.get('query',{}).get('normalized',[])}
        redir={r['from']:r['to'] for r in d.get('query',{}).get('redirects',[])}
        pages={p['title']:p for p in d.get('query',{}).get('pages',[]) if 'missing' not in p}
        for c in chunk:
            t=norm.get(c,c); t=redir.get(t,t)
            p=pages.get(t)
            if not p: continue
            ll=p.get('langlinks') or []
            out[c]={'wp_title':t,'pageid':p.get('pageid'),
                    'zh':ll[0]['title'] if ll else None}
    return out

def pageviews(wp_title, start="2025090100", end="2026083100"):
    u=("https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/en.wikipedia/all-access/user/"
       + urllib.parse.quote(wp_title.replace(' ','_'), safe='') + f"/monthly/{start}/{end}")
    d=get(u)
    if not d: return None
    items=d.get('items',[])
    if not items: return None
    return {'total':sum(x['views'] for x in items),'months':len(items)}

if __name__=='__main__':
    names=json.load(open(sys.argv[1]))
    # step 1: search-resolve each name
    resolved={}
    for i,n in enumerate(names):
        r=wp_search(n)
        if r: resolved[n]=r
        if i%25==0: print(f"search {i}/{len(names)} ok={len(resolved)}", flush=True)
    json.dump(resolved, open('wp_resolved.json','w'), ensure_ascii=False)
    print("resolved:", len(resolved))
