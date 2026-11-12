"""Resume-safe Meshy scenery production. Credentials stay in the environment."""
import concurrent.futures
import json
import os
from pathlib import Path
import threading
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
STATE = ROOT / 'work/meshy-tasks.json'
OUT = ROOT / 'assets/models/scenery'
LOCK = threading.Lock()
KEY = os.environ['MESHYAI']
BASE = 'https://api.meshy.ai/openapi/v2/text-to-3d'
ASSETS = {
    'broadleaf': ('One enormous ancient oak tree, a tall thick gnarled trunk with visible roots at its foot, long widely spreading crooked branches, sparse sallow olive foliage, a high canopy with clear open space below. Full tree from roots to crown. Grim medieval wilderness game asset. No ground platform, no pot, no scenery, no people.', 4000),
    'conifer': ('One towering old Scots pine tree, very tall straight weathered trunk, sparse irregular high branches and dark muted green needle clusters, clear exposed lower trunk. Full tree from roots to crown. Grim medieval wilderness game asset. No ground platform, no pot, no scenery, no people.', 3500),
    'bush': ('One low wild bramble bush, tangled woody stems with sparse sallow olive leaves, irregular rounded silhouette, dry and half withered. Grim medieval wilderness game asset. One isolated bush, no pot, no ground platform, no scenery.', 1600),
    'stone': ('One mineable outcrop of rough weathered grey limestone, a connected low cluster of angular cracked rocks, broad stable base, rugged natural silhouette. Grim medieval wilderness game asset. No landscape, no ground platform, no tools.', 1400),
    'iron': ('One mineable outcrop of dark grey iron ore rock, angular cracked stone with rusty orange iron veins and a few dull metallic inclusions. Low connected rocky outcrop. Grim medieval wilderness game asset. No landscape, no ground platform, no tools.', 1400),
    'gold': ('One mineable outcrop of grey quartz rock with clearly visible small ochre gold veins and golden mineral inclusions. A low connected angular rocky outcrop. Grim medieval wilderness game asset. Natural raw ore, no gold bars, no landscape, no ground platform.', 1400),
    'boulder': ('One massive ancient irregular granite boulder, rounded angular silhouette, deeply weathered fractured surface, ash grey stone with subtle dark moss in cracks. Grim medieval wilderness game asset. One isolated rock, no ground platform, no landscape.', 1800),
    'tuft': ('One small clump of dry wild grass, several thin curved blades growing from one central root, sallow olive and straw brown. Three dimensional geometry, readable tapered blades. Grim medieval wilderness game asset. No pot, no ground platform, no landscape.', 800),
    'flower': ('One small patch of wild meadow flowers, five tiny faded bone white flowers with muted yellow centres on thin stalks with sparse olive leaves, growing from a single compact clump. Three dimensional stems and petals. Grim medieval wilderness game asset. No pot, no ground platform.', 1000),
    'pebble': ('One small loose rough flint stone, irregular flattened pebble, angular chipped edges, weathered ash grey with brown patina. Grim medieval survival game collectible. One isolated stone, no pile, no ground platform, no landscape.', 600),
    'reed': ('One compact clump of tall marsh reeds, slender upright stalks, dry brown seed heads and long muted olive blades. Three dimensional stalks. Grim medieval wilderness game asset. No water, no pot, no ground platform, no landscape.', 1000),
    'branch': ('One fallen forked oak branch lying horizontally, short broken twigs, rough dark umber bark, pale splintered wood at broken ends, no leaves. Grim medieval survival game collectible. One isolated branch, no ground platform, no scenery.', 900),
    'stump': ('One large freshly chopped old oak stump, broad gnarled roots, thick rough dark brown bark, pale weathered flat cut top with visible growth rings. Short massive stump, matching a huge ancient oak tree. Grim medieval wilderness game asset. No ground platform, no scenery.', 1600),
    'rubble': ('One small connected pile of broken angular stone rubble and chipped grey rock fragments left after mining an outcrop, low wide silhouette. Grim medieval wilderness game asset. No ground platform, no landscape, no tools.', 1000),
    'chest': ('One closed medieval wooden storage chest, rectangular sturdy oak plank box with a slightly rounded hinged lid, dark iron straps, a simple iron front latch, small feet. Weathered umber wood and dull black iron. Grim medieval survival game prop. No treasure, no pedestal, no scenery.', 2400),
}
STYLE = 'Weathered natural materials with clearly readable fine surface detail. Muted but visible colours: grey stone, umber bark, olive leaves, rusty orange iron and ochre gold. Hand painted medieval wilderness game asset, neutral even lighting, no painted shadows, no text.'
state = json.loads(STATE.read_text()) if STATE.exists() else {}

def save():
    STATE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE.with_suffix('.tmp')
    tmp.write_text(json.dumps(state, indent=2) + '\n')
    tmp.replace(STATE)

def request(method, url, body=None):
    headers = {'Authorization': 'Bearer ' + KEY}
    data = None
    if body is not None:
        headers['Content-Type'] = 'application/json'
        data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=60 if method == 'POST' else 30) as response:
        return json.load(response), response.headers

def task(name, stage, payload):
    with LOCK:
        entry = state.setdefault(name, {})
        ident = entry.get(stage + '_id')
        result = entry.get(stage + '_result')
    if result and result.get('status') == 'SUCCEEDED':
        return result
    if not ident:
        result, _ = request('POST', BASE, payload)
        ident = result['result']
        with LOCK:
            entry[stage + '_id'] = ident
            entry[stage + '_request'] = payload
            save()
        print(name, stage, 'submitted', ident, flush=True)
    while True:
        try:
            result, headers = request('GET', BASE + '/' + ident)
        except urllib.error.HTTPError as error:
            if error.code in (429, 500, 502, 503, 504):
                time.sleep(10)
                continue
            raise
        except (TimeoutError, urllib.error.URLError):
            time.sleep(10)
            continue
        with LOCK:
            entry[stage + '_result'] = result
            save()
        if result['status'] == 'SUCCEEDED':
            print(name, stage, 'succeeded', flush=True)
            return result
        if result['status'] in ('FAILED', 'CANCELED'):
            raise RuntimeError(name + ' ' + stage + ': ' + json.dumps(result.get('task_error')))
        time.sleep(max(5, float(headers.get('Retry-After', 5))))

def generate(name):
    OUT.mkdir(parents=True, exist_ok=True)
    target = OUT / (name + '.glb')
    if target.exists():
        return name + ' already downloaded'
    prompt, triangles = ASSETS[name]
    preview = task(name, 'preview', {
        'mode': 'preview', 'prompt': prompt, 'ai_model': 'meshy-7.1',
        'should_remesh': True, 'target_polycount': triangles,
        'topology': 'triangle', 'target_formats': ['glb'],
    })
    refined = task(name, 'refine', {
        'mode': 'refine', 'preview_task_id': preview['id'],
        'ai_model': 'meshy-7.1', 'texture_prompt': prompt + ' ' + STYLE,
        'enable_pbr': False, 'target_formats': ['glb'],
    })
    # Asset CDN requests do not receive the API credential.
    with urllib.request.urlopen(refined['model_urls']['glb'], timeout=120) as response:
        data = response.read()
    if data[:4] != b'glTF':
        raise RuntimeError(name + ': invalid GLB download')
    target.write_bytes(data)
    with LOCK:
        state[name]['downloaded_bytes'] = len(data)
        save()
    return name + ' downloaded ' + str(len(data)) + ' bytes'

if __name__ == '__main__':
    import sys
    names = sys.argv[1:] or list(ASSETS)
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        jobs = {pool.submit(generate, name): name for name in names}
        failed = False
        for job in concurrent.futures.as_completed(jobs):
            try:
                print(job.result(), flush=True)
            except Exception as error:
                failed = True
                print(jobs[job], 'ERROR', type(error).__name__, str(error), flush=True)
                if isinstance(error, urllib.error.HTTPError):
                    print(error.read().decode(errors='replace')[:2000], flush=True)
    sys.exit(1 if failed else 0)
