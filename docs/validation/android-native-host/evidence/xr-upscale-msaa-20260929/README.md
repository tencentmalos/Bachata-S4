# XR upscale / MSAA evidence

See [report](../../xr-upscale-msaa-20260929.md). `delivery-identity.json` is the final installed build; `identity.json` is the preceding game A/B build. Both use the same functional FDM fixes; the final source revision removes added Mesa comments and aligns the legacy dynamic PP attachment with Foundation GENERAL layout.

The final GPU tests ran with the final source-built driver/host. `upscale-delivery-validation.log` records Khronos validation actually attached. Earlier negative matrices are retained separately. `game-host-relevant.log` is a filtered excerpt, not the full session log. Full logs and the four unmodified PNGs (including the final delivery capture) stay under the local build validation directory; `screenshots.json` records their absolute paths, SHA-256 and frame/session identities. No game binaries or saves are included.
