# Compatibility entry points

The TFT source now lives in `tools/temporal-fusion-transformer`.
The task runner, container launcher, setup scripts, and CMake entry here forward
to that tool so existing commands remain usable. Use the canonical tool paths
for new work. Existing `.build/tft` artifacts are retained; canonical tasks use
`.build/temporal-fusion-transformer` to avoid reusing a moved-source CMake cache.

These compatibility files are MIT licensed.
