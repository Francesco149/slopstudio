"""Run the mock provider: `nix develop --command python -m providers.mock` (PORT env)."""
import os

import uvicorn

from . import app

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=int(os.environ.get("PORT", "8010")))
