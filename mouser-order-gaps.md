# Missing Mechanical And Passive Items

These are the parts still missing from [mouser-order-draft.csv](/home/atom/devel/cockscreen/mouser-order-draft.csv#L1) for a practical DIY build.

## User-Supplied Parts

| Qty | Item | Notes |
|---|---|---|
| 1 | ProtoSupplies 16-ch analog/digital mux module | Already owned by the user; do not order the CD74HC4067 separately |

## Mechanical And Interconnect

| Qty | Item | Notes |
|---|---|---|
| 1 | 40-pin GPIO header / connector set | Match the Waveshare board stack-up |
| 1 | 5.5 x 2.1 mm DC barrel jack, panel mount | Power input |
| 6 | Input jack | 3 direct CV inputs + 3 gate inputs |
| 4 | Output jack | One per CV output |
| 3 | 3-position switch | Direct, attenuated, bipolar modes |
| 1 | PCB or perfboard | If not using a custom board |
| 1 | Calibration header or test points | Helpful during trim and verification |
| 1 | Standoffs, screws, spacers | Mechanical mounting |
| 1 | Panel hardware set | Nuts, washers, labels, etc. |
| 1 | Hookup wire / ribbon cable | Board interconnects |
| 1 | 12 V DC wall PSU | Sized for the full system load |

## Exact Mouser Candidates Found

| Qty | Item | Mouser Number | Notes |
|---|---|---|---|
| 1 | 40-pin GPIO header / connector set | 474-PRT-13054 | 2x20 shrouded header option; 474-PRT-14275 is the male header alternative |
| 1 | 5.5 x 2.1 mm DC barrel jack, panel mount | 485-610 | Panel mount 2.1mm DC barrel jack |
| 10 | 3.5 mm mono jack | 502-35RAPC2AHN2 | Use for the input/output jack pool if you standardize on 3.5 mm mono |
| 3 | 3-position switch | 633-D22013LP | ON-OFF-ON panel-mount toggle switch |
| 4 | M3 spacer | 909-HEX-SPACER-M3X12 | Example board-mount spacer length |

## Passive Parts

| Qty | Item | Notes |
|---|---|---|
| 6 | 100k resistor | Direct CV divider pairs |
| 3 | 1k resistor | Direct CV series protection |
| 3 | 1 nF capacitor | Direct CV low-pass filter |
| 3 | 10k resistor | Gate series resistor |
| 3 | 100k resistor | Gate pull-down |
| 1 | 1k resistor | Pot mux output protection |
| 2 | 470 uF 25 V electrolytic capacitor | Input and main rail bulk capacitance |
| 2 | 100 nF film capacitor | High-frequency supply decoupling |
| 1 | 33 uH 3 A power inductor | Buck converter inductor |
| 1 | 330 uF 10 V electrolytic capacitor | Buck output bulk capacitance |
| 1 | 10 ohm 1/2 W resistor | Isolates the analog 5 V_A branch |
| 1 | 470 uF 10 V electrolytic capacitor | Analog rail reservoir |
| 1 | 100 nF C0G capacitor | Analog rail decoupling |
| 4 | 1k resistor | DAC output series protection |
| 4 | 10 nF C0G capacitor | DAC reconstruction filter |
| 4 | 10k 0.1% metal film resistor | Gain network resistor to ground |
| 4 | 9.76k 0.1% metal film resistor | Gain network feedback resistor |
| 4 | 100 nF film capacitor | Local op-amp supply decoupling |
| 1 | 10k 0.1% metal film resistor | Reference load / bias resistor |
| 0-16 | 100 nF capacitor | Optional pot smoothing per channel |

## Helpful DIY Assembly Extras

| Qty | Item | Notes |
|---|---|---|
| 5 | DIP-8 socket | For the four OPA277PA parts and AD780AN |
| 1 | DIP-14 socket | Helpful if you keep a THT Schmitt trigger |
| 1 | SMD adapter board | For SN74LVC14ANSR if you keep the SMD substitute |
| 16 | Pot knobs | Makes the pot bank usable |
| 1 | Heat-shrink assortment | Wire insulation and strain relief |
| 1 | Ferrule or crimp assortment | Improves reliability of power and panel wiring |
| 1 | Solder and flux | Basic assembly consumables |
| 1 | Solder wick or pump | Rework and cleanup |
| 1 | Wire labels or markers | Helpful for panel harnesses |

## Current Draft Coverage

Already covered in the current CSV:

| Qty | Item |
|---|---|
| 1 | MF-R110 polyfuse |
| 1 | 1N5819 Schottky diode |
| 1 | 1.5KE15A TVS diode |
| 1 | LM2596T-5.0/NOPB buck regulator |
| 1 | 1N5822 Schottky diode |
| 4 | OPA277PA |
| 1 | AD780ANZ |
| 22 | 1N4148 diodes |
| 16 | 10k linear potentiometers |
| 1 | SN74LVC14ANSR substitute |

The mux module is already owned, so it is intentionally excluded from the order CSV.