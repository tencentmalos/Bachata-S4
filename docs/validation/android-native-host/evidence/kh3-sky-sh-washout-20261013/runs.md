# KH3 sky SH washout: runs

Cold start of the Olympus save on AYN Odin3 (`6edb0593`), Render Scale 75%, `leaf_fastpath=1`; screenshot about 20 s after Continue. Brightness and colour are the mean of the centre crop (100,60)-(540,260) of the 640x360 screenshot; the screenshots themselves stay local (game art). SH = View uniform buffer word 0x990 (R terms of `SkyIrradianceEnvironmentMap[0]`, w = scaled DC term), read with `upload_diag cb_dump 44f1e20d409f43f7 1 4096`. Full values in `view-ub-sky-sh.txt`.

| Run | APK | Change | Brightness | RGB | View UB 0x990 | Picture |
|---|---|---|---:|---|---|---|
| viewub/run2 | `9474878f` | none (before the fix) | 247 | 253,242,254 | -683 -638 683 353 | washed out (white/pink) |
| viewub/run3 | `9474878f` | none | 247 | 249,247,241 | ~ -4e-7 | washed out (white) |
| viewub/run5 | `9474878f` | none | 124 | 137,124,91 | 0 0 0 0 | normal-looking (no sky light) |
| viewub/run6 | `9474878f` | none | 122 | 134,122,89 | 0 0 0 0 | normal-looking (no sky light) |
| tinywb/run1 | `b9eba66a` | write-back | 121 | 134,121,88 | 21337 21337 -21337 nan | normal-looking |
| tinywb/run2 | `b9eba66a` | write-back | 142 | 141,151,97 | nan | green cast |
| tinywb/run3 | `b9eba66a` | write-back | 121 | 133,121,88 | nan | normal-looking |
| tinywb/run4 | `b9eba66a` | write-back | 121 | 133,121,88 | nan | normal-looking |
| tinywb-dump/run1 | `42293d18` | write-back + dump | 161 | 203,136,180 | 0.029 0.011 0.265 20.5 | pink washout |
| tinytrace/run1 | `47e82ab7` | write-back + dump + record | 141 | 139,151,96 | nan | green cast |
| tinytrace/run2 | `47e82ab7` | write-back + dump + record | 121 | 134,121,89 | nan | normal-looking |
| tinytrace/run3 | `47e82ab7` | write-back + dump + record | 120 | 133,120,88 | nan | normal-looking |
| adopt/run1 | `3b4a18b4` | write-back + mip adoption, dump | 125 | 138,125,92 | 0.0243 0.0090 0.2204 0.2006 | normal |
| adopt/run2 | `3b4a18b4` | same | 124 | 136,124,91 | identical | normal |
| adopt/run3 | `3b4a18b4` | same | - | - | - | invalid: intro GPU fault #11 |
| adopt/run4 | `3b4a18b4` | same | 119 | 131,118,86 | identical | normal |
| adopt-default/run1 | `3b4a18b4` | write-back + mip adoption (defaults) | 122 | 135,123,90 | identical | normal |
| adopt-default/run2 | `3b4a18b4` | same | - | - | - | invalid: intro GPU fault #11 |
| adopt-default/run3 | `3b4a18b4` | same | 125 | 137,125,92 | identical | normal |

"Normal-looking" with SH 0 or NaN means the sky diffuse term is lost, not that the frame is correct. Before these runs, 15 of about 17 Olympus entries on 10-13 afternoon were washed out (earlier APKs, same save).
