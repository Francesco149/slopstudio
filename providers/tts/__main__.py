"""Run the Qwen3-TTS provider: `python -m providers.tts` (PORT env, default 8010)."""
import os

import uvicorn

from . import app

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=int(os.environ.get("PORT", "8010")))
