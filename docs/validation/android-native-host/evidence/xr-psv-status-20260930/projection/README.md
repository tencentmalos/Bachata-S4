# Projection follow-up evidence

Final package: `final-package.json`; device SHA: `installed-release.txt`.
Final state: `status-release.txt`, `debug-status-release.txt`.
FOV-derived ratios: `density-ratios.json`; CPU checks: `projection-tests.txt`.

Final same-session comparison (default restored to cropped afterwards):

- [Centred crop](psv-symmetric-release.jpg)
- [Full runtime FOV](psv-full-release-ab.jpg)

The following are **intermediate diagnostic images, not the delivered behavior**:

- `psv-before-crop.jpg`: original initial first-eye anchor, full FOV.
- `psv-cropped-projection.jpg` / `psv-full-projection-ab.jpg`: follow-heading candidate
  rejected by the user, virtual parallel cameras.
- `psv-fixed-cropped-final.jpg` / `psv-fixed-full-final-ab.jpg`: despite these early
  filenames, an intermediate off-centre crop, with original runtime cameras and
  fixed anchor. Its composed image position differs from full FOV.
- `psv-diag-{full,shared,symmetric,axis}.jpg`: bounded diagnostic comparison. Shared
  off-centre and axis-inclusive crops still shift the composed model; symmetric
  cropping agrees with full FOV. These images contain a brief OS capture indicator.

Pico screenshots were captured with `pico_capture_screen` and contain JPEG bytes;
archived `.jpg` copies are byte-for-byte copies, without cropping or retouching.
Capture counts and owned-device-file cleanup are recorded in `cleanup.json`.
