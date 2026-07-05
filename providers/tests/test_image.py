"""image provider tests — the deterministic model-free path (no ComfyUI needed).

SLOP_IMAGE_FAKE makes text2image write a tiny solid-color PNG instead of calling ComfyUI,
so the protocol + cache behaviour are testable without the GPU engine.
"""
import os
import struct
import time

os.environ["SLOP_IMAGE_FAKE"] = "1"  # before importing the provider

from fastapi.testclient import TestClient

from providers.image import _build_graph, app, provider


def _wait(client, job_id, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = client.get(f"/jobs/{job_id}").json()
        if r["status"] in ("done", "error"):
            return r
        time.sleep(0.05)
    raise AssertionError("job did not finish in time")


def _clear_cache():
    root = provider.cache.root
    if os.path.isdir(root):
        for f in os.listdir(root):
            try:
                os.remove(os.path.join(root, f))
            except OSError:
                pass


def test_build_graph_injects_params():
    g = _build_graph({"prompt": "gemma-san pointing", "negative": "blurry",
                      "seed": 7, "width": 832, "height": 1216, "steps": 30, "cfg": 6.0})
    assert g["6"]["inputs"]["text"] == "gemma-san pointing"
    assert g["7"]["inputs"]["text"] == "blurry"
    assert g["5"]["inputs"]["width"] == 832 and g["5"]["inputs"]["height"] == 1216
    assert g["3"]["inputs"]["seed"] == 7 and g["3"]["inputs"]["steps"] == 30
    assert g["3"]["inputs"]["cfg"] == 6.0
    # wiring intact: KSampler pulls model/positive/negative/latent from the right nodes
    assert g["3"]["inputs"]["model"] == ["4", 0]
    assert g["3"]["inputs"]["positive"] == ["6", 0]


def test_build_graph_lora_rewires_model_and_clip():
    g = _build_graph({"prompt": "gemma-san chibi", "lora": "gemma-san-chibi"})
    assert g["10"]["class_type"] == "LoraLoader"
    assert g["10"]["inputs"]["lora_name"] == "gemma-san-chibi.safetensors"
    assert g["3"]["inputs"]["model"] == ["10", 0]   # sampler pulls the LoRA-patched model
    assert g["6"]["inputs"]["clip"] == ["10", 1]     # text encoders pull the LoRA-patched clip
    base = _build_graph({"prompt": "x"})             # no lora → untouched base wiring
    assert "10" not in base and base["3"]["inputs"]["model"] == ["4", 0]


def test_capabilities():
    with TestClient(app) as client:
        caps = client.get("/capabilities").json()
        assert caps["provider"] == "image"
        t2i = next(t for t in caps["types"] if t["type"] == "text2image")
        assert "prompt" in t2i["params_schema"]["properties"]
        assert t2i["params_schema"]["required"] == ["prompt"]


def test_text2image_fake_writes_png_and_caches():
    _clear_cache()
    with TestClient(app) as client:
        body = {"type": "text2image", "params": {"prompt": "gemma-san smug", "seed": 12, "width": 48, "height": 48}}
        r = client.post("/jobs", json=body).json()
        assert r["cached"] is False
        done = _wait(client, r["job_id"])
        assert done["status"] == "done"
        a = done["result"]["assets"][0]
        assert a["kind"] == "image" and a["uri"].endswith(".png")
        path = a["uri"].replace("file://", "")
        with open(path, "rb") as f:
            sig = f.read(8)
        assert sig == b"\x89PNG\r\n\x1a\n"  # a real PNG
        assert struct.calcsize(">I")  # sanity

        r2 = client.post("/jobs", json=body).json()  # identical params → cache hit
        assert r2["hash"] == r["hash"] and r2["cached"] is True
