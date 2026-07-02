# library/ — the global golden asset library

Cross-project, reusable media browsed in the studio's **Library** panel (Tools ▸ Library).
The editor **scans these dirs** — drop a file in here on disk (or via *Add files…* in the
panel) and it appears. **Filename = display name** (rename in the panel = move the file);
search filters by name; images show thumbnails. Drag an item onto the timeline, or
double-click to drop it at the playhead.

```
library/
  images/   png jpg jpeg bmp gif        ← golden sprites, reaction pics, backgrounds, plates
  audio/    wav mp3 ogg flac m4a         ← stingers, SFX, music beds
  video/    mp4 mov webm mkv avi         ← reusable B-roll clips
```

Generation metadata (for items produced by the TTS/image providers) rides in a
`<file>.meta.json` sidecar so a library item can be **regenerated** with its original params
(the per-project library view). Sprite sheets from GPT are cut into per-image library entries
by the **Sprite-sheet** processor (Tools ▸ Sprite sheet).

The seeded starter set is the Gemma-san host reaction sprites (`gemma-*`) + the canon
backgrounds (`bg-*`). Add your own golden sprites and meme reaction pics here.
