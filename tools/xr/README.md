# PSV status model

`build_psv_model.py` builds the original PCH-1000-shaped status console in metres
(+X right, +Y up, +Z front). Run it using Blender's Python:

```sh
/Applications/Blender.app/Contents/MacOS/Blender --background \
  --python tools/xr/build_psv_model.py -- "$PWD"
```

The app asset is `android/shadps4-app/app/src/main/assets/xr/psv_status.glb`.
The editable Blender model and four inspection renders go to
`build/validation/xr-psv-status-20260930/`. These renders use Blender lighting;
they do not verify the headset's Lite Engine GI or status rendering.

Photo references and their sources are saved in
`docs/validation/android-native-host/evidence/xr-psv-status-20260930/references/`.
The envelope follows Sony's published 182 × 83.5 × 18.6 mm dimensions; this is
an independently modelled approximation, not a scanned or official CAD model.

## PlayStation symbol attribution

`playstation.svg` is Font Awesome Free 6.7.2's PlayStation brand icon, by
Fonticons, Inc. (copyright 2024), retrieved from
https://github.com/FortAwesome/Font-Awesome/blob/6.x/svgs/brands/playstation.svg .
The icon is licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
Its paths are extruded and scaled as the Home key marking. No other third-party
mesh is included. PlayStation and PS Vita marks belong to their respective owners.
