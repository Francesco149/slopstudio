#!/usr/bin/env python3
"""Submit the 3 A/B prompts to the LIVE slop-image provider (Illustrious-XL path).

Runs on wslop; talks to the deployed provider at http://lame:8011 exactly as the editor
does (POST /jobs, poll /jobs/{id}, GET /assets/{hash}.png). Saves PNGs locally for the feed.
"""
import json, time, urllib.request, sys, os

PROV = "http://lame:8011"
OUT = "/opt/src/slopstudio/build/krea-eval"

PROMPTS = [
    ("reaction", 111, "anime girl, shocked surprised face, pointing at viewer, comic reaction meme, vibrant"),
    ("background", 222, "cozy cluttered anime bedroom interior, warm lighting, detailed, no characters"),
    ("prop", 333, "a glowing enchanted sword game asset, centered, plain neutral background"),
]


def post(path, payload):
    req = urllib.request.Request(PROV + path, data=json.dumps(payload).encode(),
                                 headers={"content-type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def get(path):
    with urllib.request.urlopen(PROV + path, timeout=30) as r:
        return r.read()


os.makedirs(OUT, exist_ok=True)
for tag, seed, prompt in PROMPTS:
    t0 = time.monotonic()
    sub = post("/jobs", {"type": "text2image",
                         "params": {"prompt": prompt, "seed": seed, "width": 1024, "height": 1024},
                         "inputs": []})
    jid, h = sub["job_id"], sub["hash"]
    print(f"[illustrious:{tag}] job={jid} hash={h} cached={sub.get('cached')}", flush=True)
    status = sub.get("status")
    while status not in ("done", "error"):
        time.sleep(0.8)
        j = json.loads(get(f"/jobs/{jid}").decode())
        status = j.get("status")
    if status == "error":
        print("  ERROR:", json.dumps(j.get("error"))[:400]); continue
    png = get(f"/assets/{h}.png")
    path = f"{OUT}/illus_{tag}.png"
    with open(path, "wb") as f:
        f.write(png)
    print(f"  OK {time.monotonic()-t0:.1f}s bytes={len(png)} -> {path}", flush=True)
