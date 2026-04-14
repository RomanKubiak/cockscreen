# DIY Enclosure Option 2

This is the plywood-shell plus aluminum-top-panel version of the desktop enclosure. It keeps the project off a rack shelf and gives enough width for the control surface, jacks, and faders.

## Target Envelope

| Item | Rough Size | Notes |
|---|---|---|
| Overall outside size | 520 x 300 x 120 mm | Comfortable desktop footprint |
| Top slope | 15 degrees | Matches the existing console-style recommendation |
| Top panel | 3 mm aluminum | Main drill sheet |
| Shell | 12 mm birch plywood | Side cheeks, front, rear, and base |
| Rear service bay | Yes | Keep the Pi, power, and wiring serviceable |

## Rough Cut List

| Part | Qty | Material | Rough Size | Notes |
|---|---|---|---|---|
| Top panel | 1 | 3 mm aluminum | 520 x 300 mm | Drill all front-panel controls here |
| Bottom panel | 1 | 12 mm birch plywood | 520 x 300 mm | Mount Pi, Waveshare board, and PSU entry here |
| Side cheeks | 2 | 12 mm birch plywood | 300 x 120 mm profile | Mirror pieces with a 15 degree top slope |
| Front apron | 1 | 12 mm birch plywood | 520 x 45 mm | Optional if the top panel overlaps the front edge |
| Rear apron | 1 | 12 mm birch plywood | 520 x 85 mm | Leave removable access if possible |
| Internal cleats | 4 | Hardwood or aluminum angle | 15 x 15 x 260 mm | Support the top panel and base |

## Rough Front Panel Layout

Pots, jacks, and sliders are not to scale here. The point is zone placement and spacing.

For a visual rough sketch, see [docs/diy-enclosure-option-2-rough.svg](docs/diy-enclosure-option-2-rough.svg).

```text
Rear / top
+--------------------------------------------------------------------------------+
| DC JACK  SW     NeoSlider slot                              GATE IN 1 2 3      |
| [o]     [o]   [==================]                           [o] [o] [o]       |
|                                                                                |
| CV IN 1 2 3                          P1  P2  P3  P4                            |
| [o] [o] [o]                          o   o   o   o                             |
|                                      P5  P6  P7  P8                            |
| CV OUT 1 2 3 4                       o   o   o   o                             |
| [o] [o] [o] [o]                                                               |
|                                                                                |
|                           MOTOR FADER 1            MOTOR FADER 2               |
|                           [================]      [================]           |
+--------------------------------------------------------------------------------+
Front / bottom
```

## Hole And Placement Rules

- Keep at least 20 mm from the panel edge to the first row of holes.
- Keep pot centers around 28 to 30 mm apart.
- Keep jack centers around 20 to 22 mm apart.
- Use the actual module template for the NeoSlider slot and the two motorized fader cutouts.
- Put the Pi, power entry, and cable strain relief on a removable rear tray, not on the faceplate.

## Practical Build Order

1. Cut the plywood shell first and dry-fit the slope.
2. Make a cardboard or paper drill template for the faceplate.
3. Mark the slider slots and pot rows before drilling any holes.
4. Drill the jacks and power entry last so you can keep the layout balanced.
5. Install the Pi and power hardware on the rear tray before wiring the controls.
