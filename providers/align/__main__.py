"""Run the align provider: `python -m providers.align` (PORT env, default 8014)."""
import os

import uvicorn

from . import app

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=int(os.environ.get("PORT", "8014")))
