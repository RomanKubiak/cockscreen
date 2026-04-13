# Cockscreen BOM

This BOM covers the Pi Zero 2 W analog front end described in [ANALOG.md](ANALOG.md). It assumes one build with three direct CV inputs on AD1-AD3, one 16-channel pot bank on AD0, and three gate inputs.

All discrete add-on parts below are specified in THT or leaded packages. The Pi and Waveshare board are module assemblies, not discrete parts.

## Core platform

| Qty | Part | Notes |
|---|---|---|
| 1 | Raspberry Pi Zero 2 W | Main runtime platform |
| 1 | Waveshare ADS1256 ADC board | Analog input board used by the runtime |
| 1 | 40-pin GPIO header / connector set | Match the Waveshare board stack-up |
| 1 | Standoffs, screws, spacers | Mechanical mounting |

## Direct CV inputs

Repeat the parts below three times, once for each direct CV input on AD1, AD2, and AD3.

| Qty | Part | Notes |
|---|---|---|
| 3 | Input jack | One per CV channel |
| 6 | 100k resistor | Divider pair for 0-10 V sources |
| 3 | 1k resistor | Series protection |
| 6 | 1N4148 diode | Clamp pair to 0 V and 3.3 V, two per channel |
| 3 | 1 nF capacitor | Low-pass filter to ground |
| 0-3 | MCP6002-I/P | Optional bipolar level shift stage, DIP-8 THT dual RRIO op-amp |
| 3 | 3-position switch | Direct 0-5 V, attenuated 0-10 V, or bipolar mode |

## Pot bank on AD0

| Qty | Part | Notes |
|---|---|---|
| 1 | CD74HC4067E | 16-channel analog multiplexer, DIP-24 THT |
| 16 | 10k linear potentiometer | One per mux channel |
| 1 | 1k resistor | Series protection at mux output |
| 2 | 1N4148 diode | Clamp pair at mux output |
| 0-16 | 100 nF capacitor | Optional smoothing capacitor per pot channel |
| 1 | Breakout or PCB for mux wiring | Keeps the pot bank tidy |

## Gate inputs

Repeat the parts below three times, once for each gate input.

| Qty | Part | Notes |
|---|---|---|
| 3 | Input jack | Gate / trigger source |
| 3 | 10k resistor | Series input resistor |
| 3 | 100k resistor | Pull-down to ground |
| 6 | 1N4148 diode | Clamp pair to 0 V and 3.3 V, two per gate input |
| 1 | SN74LVC14AN | Schmitt-trigger buffer, DIP-14 THT |

## Power Input And Distribution

All rails for the project come from a 12 V wall PSU. The Pi is fed from a regulated 5 V rail, and the precision analog section gets its own filtered 5 V_A branch.

The detailed power distribution schematic is in [docs/cv-power-input-stage.svg](docs/cv-power-input-stage.svg).

| Qty | Part | Notes |
|---|---|---|
| 1 | 12 V DC wall PSU | Sized for the full system load |
| 1 | 5.5 x 2.1 mm DC barrel jack, panel mount | THT power input |
| 1 | MF-R110 polyfuse | Input protection on the 12 V rail |
| 1 | 1N5819 Schottky diode | Reverse polarity protection |
| 1 | 1.5KE15A TVS diode | Input surge clamp |
| 2 | 470 uF 25 V electrolytic capacitor | Input and main-rail bulk capacitance |
| 2 | 100 nF film capacitor | High-frequency supply decoupling |
| 1 | LM2596T-5.0 | 12 V to 5 V buck regulator, TO-220-5 THT |
| 1 | 33 uH 3 A power inductor | Buck converter inductor |
| 1 | 1N5822 Schottky diode | Buck converter catch diode |
| 1 | 330 uF 10 V electrolytic capacitor | Buck output bulk capacitance |
| 1 | 10 ohm 1/2 W resistor | Isolates the analog 5 V_A branch |
| 1 | 470 uF 10 V electrolytic capacitor | Analog rail reservoir |
| 1 | 100 nF C0G capacitor | Analog rail decoupling |

## Precision CV Output Stage

This section is the DAC output conditioning path used for microtuning CVs. The board exposes four DAC channels (DAC0-DAC3). Each high-resolution DAC output around 0-2.5 V is then filtered, buffered, and scaled to a stable 0-5 V jack output.

The detailed output schematic is in [docs/cv-output-stage.svg](docs/cv-output-stage.svg).

| Qty | Part | Notes |
|---|---|---|
| 4 | Output jack | One per CV channel |
| 4 | 1k resistor | Series DAC input / output protection |
| 4 | 10 nF C0G capacitor | DAC reconstruction filter, low drift |
| 4 | 10k 0.1% metal film resistor | Gain network resistor to ground |
| 4 | 9.76k 0.1% metal film resistor | Gain network resistor in feedback path |
| 4 | 3296W-1-501 multiturn trimmer | Fine gain trim per channel |
| 8 | 1N4148 diode | Clamp pair at each CV output, two per channel |
| 4 | 100 nF film capacitor | Local op-amp supply decoupling |
| 2 | MCP6V28-I/P | Dual zero-drift RRIO op-amp, DIP-8 THT |
| 1 | AD780AN | Precision 2.5 V reference, DIP-8 THT |
| 1 | 10 k 0.1% metal film resistor | Reference load / bias resistor |
| 1 | Calibration header or test points | Helpful when trimming output voltage accuracy |

## Miscellaneous

| Qty | Part | Notes |
|---|---|---|
| 1 | PCB or perfboard | If you are not using a custom board |
| 1 | Hookup wire / ribbon cable | Board interconnects |
| 1 | Panel hardware set | Jack nuts, washers, labels, etc. |

## Notes

- Keep all analog and gate grounds common with the Pi and the Waveshare board.
- Do not feed negative voltages directly into the ADC path.
- Do not connect raw 5 V gate signals straight to Pi GPIO.
- The precision output stage assumes the DAC source is a high-resolution digital section elsewhere in the project; this BOM covers the analog conditioning, reference, and power distribution around it.
- For a single-channel prototype, use MCP6V27-I/P; for the 4-channel build, MCP6V28-I/P is the recommended dual-package option.
