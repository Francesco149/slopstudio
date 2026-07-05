"""slopstudio generation providers.

Out-of-process HTTP/WS services wrapping each model, all speaking the slopstudio
provider protocol (docs/PROVIDER_PROTOCOL.md). `base` is the reusable skeleton;
concrete providers (tts, image, video, music, align) build on it. `mock` is a
model-free reference provider for developing the editor against a live endpoint.
"""
