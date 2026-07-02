"""align provider tests — exercised on the deterministic model-free path.

SLOP_ALIGN_FAKE forces the no-GPU/no-binary path so word_timing/visemes are testable
without WhisperX or the Rhubarb binary; the real engines share the same handler shape.
"""
import json
import os
import time

os.environ["SLOP_ALIGN_FAKE"] = "1"  # before importing the provider's handlers

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from providers.align import _OPENNESS, _even_word_timings, app, provider
from providers.base import param_hash


def _wav(tmp_path, dur=1.0, sr=24000):
    p = str(tmp_path / "vo.wav")
    t = np.arange(int(sr * dur)) / sr
    sf.write(p, (0.05 * np.sin(2 * np.pi * 200 * t)).astype("float32"), sr)
    return p


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


def test_openness_mapping_is_monotonic_at_the_extremes():
    assert _OPENNESS["X"] == 0.0 and _OPENNESS["A"] == 0.0  # rest / closed
    assert _OPENNESS["D"] == 1.0                            # widest
    assert 0.0 < _OPENNESS["B"] < _OPENNESS["C"] < _OPENNESS["D"]


def test_even_word_timings_partition_the_clip():
    wt = _even_word_timings("one two three four", 2.0)
    assert [w["w"] for w in wt] == ["one", "two", "three", "four"]
    assert wt[0]["t0"] == 0.0 and abs(wt[-1]["t1"] - 2.0) < 1e-6
    for a, b in zip(wt, wt[1:]):  # contiguous, monotonic
        assert abs(a["t1"] - b["t0"]) < 1e-6
    assert _even_word_timings("", 2.0) == []


def test_capabilities_lists_both_align_types():
    with TestClient(app) as client:
        caps = client.get("/capabilities").json()
        assert caps["provider"] == "align"
        types = {t["type"] for t in caps["types"]}
        assert {"word_timing", "visemes"} <= types
        wt = next(t for t in caps["types"] if t["type"] == "word_timing")
        assert wt["returns_timings"] is True


def test_word_timing_fake_path(tmp_path):
    _clear_cache()
    audio = _wav(tmp_path, dur=2.0)
    with TestClient(app) as client:
        body = {"type": "word_timing",
                "params": {"text": "fufu welcome back", "language": "en"},
                "inputs": [{"hash": "a_vo", "uri": f"file://{audio}"}]}
        r = client.post("/jobs", json=body).json()
        assert r["cached"] is False
        done = _wait(client, r["job_id"])
        assert done["status"] == "done"
        a = done["result"]["assets"][0]
        assert a["kind"] == "data" and a["uri"].endswith(".json")
        wt = a["meta"]["word_timings"]
        assert [w["w"] for w in wt] == ["fufu", "welcome", "back"]
        assert abs(wt[-1]["t1"] - 2.0) < 1e-6
        assert os.path.isfile(a["uri"].replace("file://", ""))


def test_visemes_fake_path_covers_the_clip(tmp_path):
    _clear_cache()
    audio = _wav(tmp_path, dur=1.5)
    with TestClient(app) as client:
        body = {"type": "visemes",
                "params": {"dialog": "fufu welcome back"},
                "inputs": [{"hash": "a_vo", "uri": f"file://{audio}"}]}
        r = client.post("/jobs", json=body).json()
        done = _wait(client, r["job_id"])
        assert done["status"] == "done"
        a = done["result"]["assets"][0]
        assert a["meta"]["engine"] == "fake" and a["meta"]["shape_set"] == "preston-blair"
        assert a["meta"]["n"] >= 1  # light meta; full track lives in the json file
        vis = json.load(open(a["uri"].replace("file://", "")))["visemes"]
        assert vis[0]["t0"] == 0.0 and abs(vis[-1]["t1"] - 1.5) < 1e-6
        for v in vis:  # contiguous, monotonic, openness present
            assert "openness" in v and v["t1"] >= v["t0"]
        for a, b in zip(vis, vis[1:]):
            assert abs(a["t1"] - b["t0"]) < 1e-6


def test_inputs_fold_into_the_job_hash(tmp_path):
    _clear_cache()
    audio = _wav(tmp_path, dur=1.0)
    with TestClient(app) as client:
        body = {"type": "visemes", "params": {"dialog": "hi"},
                "inputs": [{"hash": "a_one", "uri": f"file://{audio}"}]}
        r1 = client.post("/jobs", json=body).json()
        _wait(client, r1["job_id"])
        r2 = client.post("/jobs", json=body).json()  # identical → cache hit
        assert r2["hash"] == r1["hash"] and r2["cached"] is True

        body2 = dict(body, inputs=[{"hash": "a_two", "uri": f"file://{audio}"}])
        r3 = client.post("/jobs", json=body2).json()  # different input hash → different job
        assert r3["hash"] != r1["hash"]


def test_param_hash_includes_sorted_inputs():
    a = param_hash("visemes", {"dialog": "hi"}, ["a_one"], "v")
    b = param_hash("visemes", {"dialog": "hi"}, ["a_two"], "v")
    assert a != b  # input hash is part of the cache key
