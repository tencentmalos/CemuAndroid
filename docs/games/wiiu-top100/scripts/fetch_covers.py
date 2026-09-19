"""按 GameTDB ID 下载封面。用 curl（urllib 对该 CDN 会超时）。"""
import os, re, subprocess

REGION_DIRS = {'E':['US','EN'],'P':['EN','US'],'J':['JA'],'K':['KO'],'W':['EN','US']}

def _curl(url, out):
    r = subprocess.run(['curl','-sL','--max-time','30','-o',out,
                        '-w','%{http_code}', url],
                       capture_output=True, text=True)
    code = (r.stdout or '').strip()
    if code == '200' and os.path.exists(out) and os.path.getsize(out) > 1500:
        return True
    if os.path.exists(out) and os.path.getsize(out) <= 1500:
        os.remove(out)
    return False

def fetch(gids, style, ext, outdir, name):
    """返回 (本地路径, 来源URL) 或 (None, None)。"""
    os.makedirs(outdir, exist_ok=True)
    safe = re.sub(r'[^\w\-. ]', '_', name)[:70].strip()
    path = os.path.join(outdir, f"{safe}.{ext}")
    if os.path.exists(path) and os.path.getsize(path) > 1500:
        return path, 'cached'
    seen = []
    for gid in gids:
        dirs = REGION_DIRS.get(gid[3] if len(gid) > 3 else 'E', [])
        for d in dirs + ['US','EN','JA']:
            if (gid, d) in seen: continue
            seen.append((gid, d))
            url = f"https://art.gametdb.com/wiiu/{style}/{d}/{gid}.{ext}"
            if _curl(url, path):
                return path, url
    return None, None
