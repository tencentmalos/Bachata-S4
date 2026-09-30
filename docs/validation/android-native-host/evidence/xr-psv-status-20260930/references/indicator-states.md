# PCH-1000 indicator reference

Sony's [hardware part names](https://manuals.playstation.net/document/gb/psvita/basic/partnames.html),
[power guide](https://manuals.playstation.net/document/en/psvita/basic/power.html), and
[charging guide](https://manuals.playstation.net/document/en/psvita/basic/charge.html)
were consulted on 2026-09-30.

The upper-right front circle is the front camera. The PCH-1000 status indicator is
inside the lower-left PS key. It is blue while powered on, including external
power; flashes blue while entering standby; and slowly flashes blue for new
notifications. In standby or powered off, charging is orange, with orange
flashing if there is insufficient charge to power on. Charging completion turns
that light off. The PCH-2000 uses a different indicator arrangement and is not
the model used here.

Simulation: XR focus maps to active/blue. Losing focus shows a 1.2-second standby
transition, then orange if Foundation's current battery sample reports charging,
or off otherwise. A stale battery sample does not assert charging. The active
state takes priority over charging. No notifications or critical power-on state
are inferred from frame rate, ordinary warnings or an invented percentage
threshold; those two states are available only as explicit visual previews.

0.4-second blinking and a 2.4-second smooth notification pulse are chosen visual
approximations, not measured Sony firmware timings. A small point light (0.002 cd,
0.15 m range) and the PS glyph's emissive material share one evaluated colour and
brightness. The right camera is never emissive. The light remains on the PS key
when the model moves or is recentered.
