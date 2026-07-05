"""rembg provider tests — exercised on the deterministic model-free path.

SLOP_REMBG_FAKE forces the corner-key matte (no ONNX/onnxruntime), so the asset write +
alpha + dims + content-hash caching are testable without the real segmenter; the real
isnet-anime engine shares the same handler shape.
"""
import base64
import io
import os
import time

os.environ["SLOP_REMBG_FAKE"] = "1"  # before importing the provider's handlers

import numpy as np
from fastapi.testclient import TestClient
from PIL import Image

from providers.base import param_hash
from providers.rembg import app, provider


def _png_b64(bg=(255, 255, 255), fg=(40, 200, 90), size=64):
    """A flat-bg image with a centred fg square → base64 PNG."""
    a = np.zeros((size, size, 4), dtype="uint8")
    a[:, :, :3] = bg
    a[:, :, 3] = 255
    q = size // 4
    a[q:size - q, q:size - q, :3] = fg
    buf = io.BytesIO()
    Image.fromarray(a, "RGBA").save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode()


def _clear_cache():
    root = provider.cache.root
    if os.path.isdir(root):
        for f in os.listdir(root):
            try:
                os.remove(os.path.join(root, f))
            except OSError:
                pass


def _wait(client, job_id, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = client.get(f"/jobs/{job_id}").json()
        if r["status"] in ("done", "error"):
            return r
        time.sleep(0.05)
    raise AssertionError("job did not finish in time")


def test_capabilities_advertise_remove_bg_and_models():
    with TestClient(app) as client:
        caps = client.get("/capabilities").json()
        assert caps["provider"] == "rembg"
        t = next(t for t in caps["types"] if t["type"] == "remove_bg")
        assert "image_b64" in t["params_schema"]["properties"]
        assert "isnet-anime" in t["params_schema"]["properties"]["model"]["enum"]


def test_remove_bg_keys_flat_background_to_alpha_and_keeps_dims():
    _clear_cache()
    with TestClient(app) as client:
        r = client.post("/jobs", json={"type": "remove_bg",
                                       "params": {"image_b64": _png_b64(), "model": "isnet-anime"}}).json()
        assert r["status"] in ("queued", "done")
        res = _wait(client, r["job_id"])
        assert res["status"] == "done", res
        asset = res["result"]["assets"][0]
        assert asset["kind"] == "image" and asset["meta"]["w"] == 64 and asset["meta"]["h"] == 64
        # fetch the cutout bytes and check: corners transparent (bg keyed), centre opaque (fg kept)
        png = client.get(f"/assets/{r['hash']}.png").content
        out = np.asarray(Image.open(io.BytesIO(png)).convert("RGBA"))
        assert out.shape == (64, 64, 4)
        assert out[0, 0, 3] == 0 and out[0, -1, 3] == 0          # flat bg → transparent
        assert out[32, 32, 3] == 255                              # subject → opaque


def test_identical_image_and_model_is_a_cache_hit():
    _clear_cache()
    b64 = _png_b64()
    with TestClient(app) as client:
        first = client.post("/jobs", json={"type": "remove_bg",
                                           "params": {"image_b64": b64, "model": "isnet-anime"}}).json()
        _wait(client, first["job_id"])
        again = client.post("/jobs", json={"type": "remove_bg",
                                           "params": {"image_b64": b64, "model": "isnet-anime"}}).json()
        assert again["cached"] is True and again["hash"] == first["hash"]


def test_hash_depends_on_model_choice():
    b64 = _png_b64()
    h_anime = param_hash("remove_bg", {"image_b64": b64, "model": "isnet-anime"}, [], provider.version)
    h_general = param_hash("remove_bg", {"image_b64": b64, "model": "u2net"}, [], provider.version)
    assert h_anime != h_general  # different model ⇒ different output ⇒ different cache key


def test_missing_image_errors_cleanly():
    _clear_cache()
    with TestClient(app) as client:
        r = client.post("/jobs", json={"type": "remove_bg", "params": {"model": "isnet-anime"}}).json()
        res = _wait(client, r["job_id"])
        assert res["status"] == "error" and res["error"]["code"] == "internal"
