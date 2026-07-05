"""Exercises the provider protocol end-to-end against the mock provider."""
import os
import time

from fastapi.testclient import TestClient

from providers.base import param_hash
from providers.mock import app, provider


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


def test_param_hash_is_stable_and_param_sensitive():
    h1 = param_hash("speech", {"text": "hi", "seed": 1}, [], "v")
    h2 = param_hash("speech", {"text": "hi", "seed": 1}, [], "v")
    h3 = param_hash("speech", {"text": "bye", "seed": 1}, [], "v")
    assert h1 == h2
    assert h1 != h3
    assert h1.startswith("a_") and len(h1) == 18  # "a_" + 16 chars


def test_volatile_keys_excluded_from_hash():
    a = param_hash("speech", {"text": "hi", "ui_label": "x"}, [], "v", volatile_keys=("ui_label",))
    b = param_hash("speech", {"text": "hi", "ui_label": "y"}, [], "v", volatile_keys=("ui_label",))
    assert a == b


def test_healthz_and_capabilities():
    with TestClient(app) as client:
        assert client.get("/healthz").text == "ok"
        caps = client.get("/capabilities").json()
        assert caps["provider"] == "mock-tts"
        speech = next(t for t in caps["types"] if t["type"] == "speech")
        assert "text" in speech["params_schema"]["properties"]


def test_job_lifecycle_then_cache_hit():
    _clear_cache()
    with TestClient(app) as client:
        body = {"type": "speech", "params": {"text": "Fufu welcome back", "seed": 3}}

        r = client.post("/jobs", json=body).json()
        assert r["status"] in ("queued", "running", "done")
        assert r["cached"] is False  # cleared cache → first run computes

        done = _wait(client, r["job_id"])
        assert done["status"] == "done"
        a = done["result"]["assets"][0]
        assert a["kind"] == "audio"
        assert a["meta"]["duration"] > 0
        assert len(a["meta"]["word_timings"]) == 3  # "Fufu welcome back"
        assert os.path.isfile(a["uri"].replace("file://", ""))

        # identical params → same hash → immediate cache hit, no recompute
        r2 = client.post("/jobs", json=body).json()
        assert r2["hash"] == r["hash"]
        assert r2["cached"] is True
        assert r2["status"] == "done"


def test_unknown_type_is_rejected():
    with TestClient(app) as client:
        r = client.post("/jobs", json={"type": "nope", "params": {}})
        assert r.status_code == 400
