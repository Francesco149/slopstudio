"""Reference provider skeleton — the slopstudio provider protocol in code.

Every concrete provider registers one or more `TypeSpec`s (a capability + its async
handler + a JSON-Schema for its params) on a `Provider`, then serves `make_app(provider)`
with uvicorn. The skeleton handles the whole protocol: capability discovery, the
content-addressed cache (idempotent by param-hash), an async bounded job queue, WebSocket
progress streaming, polling, and asset serving. See docs/PROVIDER_PROTOCOL.md.
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import os
import time
from contextlib import asynccontextmanager
from dataclasses import asdict, dataclass, field
from typing import Awaitable, Callable, Optional

from fastapi import FastAPI, HTTPException, WebSocket
from fastapi.responses import FileResponse, PlainTextResponse


# ── param-hash (the cache key) ─────────────────────────────────────────────
def _canonical(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def param_hash(provider_type, params, input_hashes, model_version, volatile_keys=()):
    """Content hash per docs/PROVIDER_PROTOCOL.md §param-hash.

    Identical (type, params-minus-volatile, sorted inputs, model_version) → identical
    hash → cache hit → no recompute. model_version in the key cleanly invalidates stale
    assets when a model is upgraded.
    """
    vol = set(volatile_keys)
    p = {k: v for k, v in params.items() if k not in vol}
    payload = {
        "provider_type": provider_type,
        "params": p,
        "inputs": sorted(input_hashes or []),
        "model_version": model_version,
    }
    digest = hashlib.sha256(_canonical(payload)).digest()
    b32 = base64.b32encode(digest).decode().lower().rstrip("=")
    return "a_" + b32[:16]


# ── result / asset ─────────────────────────────────────────────────────────
@dataclass
class Asset:
    kind: str  # image | audio | video | data | preset
    uri: str
    meta: dict = field(default_factory=dict)


@dataclass
class Result:
    assets: list = field(default_factory=list)
    timings: dict = field(default_factory=dict)
    provenance: Optional[dict] = None

    def to_json(self) -> dict:
        return {
            "assets": [asdict(a) for a in self.assets],
            "timings": dict(self.timings),
            "provenance": self.provenance,
        }


# ── capability spec ────────────────────────────────────────────────────────
Handler = Callable[[dict, list, "JobCtx"], Awaitable[Result]]


@dataclass
class TypeSpec:
    type: str
    handler: Handler
    params_schema: dict
    outputs: list = field(default_factory=list)
    deterministic: bool = True
    presets: list = field(default_factory=list)
    volatile_keys: tuple = ()
    returns_timings: bool = False


# ── job ────────────────────────────────────────────────────────────────────
class Job:
    def __init__(self, job_id: str, hash: str, type: str):
        self.job_id = job_id
        self.hash = hash
        self.type = type
        self.status = "queued"  # queued | running | done | error
        self.progress = 0.0
        self.message = ""
        self.result: Optional[dict] = None
        self.error: Optional[dict] = None
        self._subs: list[asyncio.Queue] = []
        self._payload = None

    def subscribe(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue()
        self._subs.append(q)
        return q

    def unsubscribe(self, q: asyncio.Queue):
        if q in self._subs:
            self._subs.remove(q)

    async def publish(self, frame: dict):
        for q in list(self._subs):
            await q.put(frame)

    def view(self) -> dict:
        return {
            "job_id": self.job_id,
            "hash": self.hash,
            "status": self.status,
            "progress": self.progress,
            "message": self.message,
            "result": self.result,
            "error": self.error,
        }


class JobCtx:
    """Passed to a handler so it can report progress and reach the cache."""

    def __init__(self, job: Job, cache: "AssetCache"):
        self.job = job
        self.cache = cache

    async def report(self, progress: float, message: str = ""):
        self.job.progress = float(progress)
        self.job.message = message
        await self.job.publish(
            {"status": self.job.status, "progress": self.job.progress, "message": message}
        )


# ── content-addressed cache ────────────────────────────────────────────────
class AssetCache:
    def __init__(self, root: str):
        self.root = os.path.abspath(root)
        os.makedirs(self.root, exist_ok=True)

    def _result_path(self, h: str) -> str:
        return os.path.join(self.root, f"{h}.result.json")

    def has(self, h: str) -> bool:
        return os.path.exists(self._result_path(h))

    def load(self, h: str) -> dict:
        with open(self._result_path(h)) as f:
            return json.load(f)

    def store(self, h: str, result_json: dict):
        tmp = self._result_path(h) + ".tmp"
        with open(tmp, "w") as f:
            json.dump(result_json, f)
        os.replace(tmp, self._result_path(h))  # atomic publish → crash-safe cache

    def asset_path(self, h: str, ext: str) -> str:
        return os.path.join(self.root, f"{h}.{ext}")

    def uri(self, h: str, ext: str) -> str:
        return "file://" + self.asset_path(h, ext)


# ── provider ───────────────────────────────────────────────────────────────
class Provider:
    def __init__(self, name: str, version: str, cache_dir: str = "cache", concurrency: int = 2):
        self.name = name
        self.version = version
        self.cache = AssetCache(cache_dir)
        self.types: dict[str, TypeSpec] = {}
        self.jobs: dict[str, Job] = {}
        # Created in start() so it binds to the app's running loop, not import-time.
        self._queue: Optional[asyncio.Queue] = None
        self._concurrency = concurrency
        self._workers: list[asyncio.Task] = []
        self._seq = 0

    def register(self, spec: TypeSpec) -> "Provider":
        self.types[spec.type] = spec
        return self

    def capabilities(self) -> dict:
        return {
            "provider": self.name,
            "version": self.version,
            "types": [
                {
                    "type": t.type,
                    "outputs": t.outputs,
                    "params_schema": t.params_schema,
                    "presets": t.presets,
                    "deterministic": t.deterministic,
                    "returns_timings": t.returns_timings,
                }
                for t in self.types.values()
            ],
        }

    def _new_id(self) -> str:
        self._seq += 1
        return f"j_{self._seq}"

    async def submit(self, type: str, params: dict, inputs=None, preset=None):
        if type not in self.types:
            raise KeyError(type)
        spec = self.types[type]
        inputs = inputs or []
        input_hashes = [i if isinstance(i, str) else (i or {}).get("hash") for i in inputs]
        h = param_hash(type, params, input_hashes, self.version, spec.volatile_keys)
        job = Job(self._new_id(), h, type)
        self.jobs[job.job_id] = job
        if self.cache.has(h):  # cache hit → immediate result
            job.status = "done"
            job.progress = 1.0
            job.result = self.cache.load(h)
            return job, True
        job._payload = (spec, params, inputs)
        await self._queue.put(job)
        return job, False

    async def _worker(self):
        while True:
            job = await self._queue.get()
            try:
                spec, params, inputs = job._payload
                job.status = "running"
                await job.publish({"status": "running", "progress": 0.0, "message": "start"})
                ctx = JobCtx(job, self.cache)
                t0 = time.monotonic()
                result = await spec.handler(params, inputs, ctx)
                rj = result.to_json()
                rj.setdefault("timings", {})["run_ms"] = int((time.monotonic() - t0) * 1000)
                self.cache.store(job.hash, rj)
                job.result = rj
                job.status = "done"
                job.progress = 1.0
                await job.publish({"status": "done", "progress": 1.0, "result": rj})
            except asyncio.CancelledError:
                raise
            except Exception as e:  # fault isolation: a handler crash never kills the server
                job.status = "error"
                job.error = {"code": "internal", "message": str(e), "retriable": False}
                await job.publish({"status": "error", "error": job.error})
            finally:
                self._queue.task_done()

    async def start(self):
        self._queue = asyncio.Queue()
        self._workers = [asyncio.create_task(self._worker()) for _ in range(self._concurrency)]

    async def stop(self):
        for w in self._workers:
            w.cancel()
        for w in self._workers:
            try:
                await w
            except asyncio.CancelledError:
                pass


def make_app(provider: Provider) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        await provider.start()
        try:
            yield
        finally:
            await provider.stop()

    app = FastAPI(title=f"slopstudio provider: {provider.name}", lifespan=lifespan)

    @app.get("/healthz", response_class=PlainTextResponse)
    async def healthz():
        return "ok"

    @app.get("/capabilities")
    async def capabilities():
        return provider.capabilities()

    @app.post("/jobs")
    async def post_jobs(body: dict):
        type = body.get("type")
        if type not in provider.types:
            raise HTTPException(status_code=400, detail=f"unknown type: {type}")
        job, cached = await provider.submit(
            type, body.get("params", {}), body.get("inputs"), body.get("preset")
        )
        resp = {"job_id": job.job_id, "hash": job.hash, "status": job.status, "cached": cached}
        if cached:
            resp["result"] = job.result
        return resp

    @app.get("/jobs/{job_id}")
    async def get_job(job_id: str):
        job = provider.jobs.get(job_id)
        if not job:
            raise HTTPException(status_code=404, detail="no such job")
        return job.view()

    @app.websocket("/jobs/{job_id}/events")
    async def job_events(websocket: WebSocket, job_id: str):
        await websocket.accept()
        job = provider.jobs.get(job_id)
        if not job:
            await websocket.close(code=1008)
            return
        await websocket.send_json({"status": job.status, "progress": job.progress})
        if job.status in ("done", "error"):
            if job.result:
                await websocket.send_json({"status": "done", "result": job.result})
            await websocket.close()
            return
        q = job.subscribe()
        try:
            while True:
                frame = await q.get()
                await websocket.send_json(frame)
                if frame.get("status") in ("done", "error"):
                    break
        finally:
            job.unsubscribe(q)
            await websocket.close()

    @app.get("/assets/{name}")
    async def get_asset(name: str):
        # name is "<hash>.<ext>" or "<hash>.result.json"; confine to the cache dir
        path = os.path.normpath(os.path.join(provider.cache.root, name))
        if not path.startswith(provider.cache.root) or not os.path.isfile(path):
            raise HTTPException(status_code=404, detail="no such asset")
        return FileResponse(path)

    return app
