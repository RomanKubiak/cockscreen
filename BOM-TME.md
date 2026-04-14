# TME Electronics BOM

This is the TME-focused sourcing list for the current project.

Mechanical and panel-only items are intentionally omitted here: enclosure, standoffs, screws, panel hardware, knobs, manual pots, motorized faders, NeoSlider module, wire, perfboard, jacks, barrel jack, GPIO header, and switches.

The goal is to keep this BOM to the PCB-level electronics that can be checked on TME.

## Verified TME Parts

| Qty | Function | TME part | Status | Notes |
|---|---|---|---|---|
| 1 | Input polyfuse | MF-R110 | exact, in stock | Bourns THT polymer fuse; 1055 in TME stock |
| 1 | Reverse-polarity diode | 1N5819-DIO | exact, in stock | THT DO15 Schottky; 47044 in TME stock |
| 1 | Surge clamp TVS | 1.5KE15A | exact, in stock | STMicroelectronics THT TVS; 1017 in TME stock |
| 1 | 5 V buck regulator | LM2596T-5.0-TT | TME substitute, in stock | TAEJIN / HTC Korea THT TO-220-5; 684 in TME stock |
| 1 | Buck catch diode | 1N5822-ST | exact, in stock | THT DO201AD Schottky; 4220 in TME stock |
| 1 | 33 uH buck inductor | DPU033A3 / DPO-3.0-33 | exact, in stock | THT toroidal inductor; 745 in TME stock |
| 1 | Analog mux | 74HC4067PW.118 | TME substitute, in stock | Closest stocked 16-channel mux I found; verify footprint before committing |
| 1 | H-bridge driver | SN754410NE | exact, in stock | DIP16; 291 in TME stock |
| 1 | Gate inverter | SN74HCT14N | exact, in stock | DIP14 Schmitt-trigger inverter; 46 in TME stock |
| 4 | CV output op amp | OPA277PA | exact, in stock | DIP8 precision op amp; 332 in TME stock |
| 0-3 | Optional bipolar input op amp | MCP6002-I/P | exact, in stock | DIP8 dual RRIO op amp; 1714 in TME stock |
| 1 | 2.5 V reference | LM4040AIZ-2.5/NOPB | exact, in stock | THT TO-92 fixed reference; 438 in TME stock |
| 4 | Output gain trimmer | 3296W-1-501LF | exact, in stock | 500 ohm multiturn trimmer; 452 in TME stock |
| 22 | Small-signal clamp diode | 1N4148-DIO | exact, in stock | THT DO35 switching diode; 326373 in TME stock |

## Generic Passives Still Needed

| Qty | Value / Type | Notes |
|---|---|---|
| 9 | 100k resistor | Direct CV dividers and gate pull-downs |
| 11 | 1k resistor | Series protection and mux output |
| 8 | 10k resistor | Gate series, gain network, and reference load |
| 4 | 9.76k 0.1% resistor | Output-stage feedback network |
| 1 | 10k 0.1% resistor | Reference load / bias resistor |
| 1 | 10 ohm 1/2 W resistor | Analog rail isolation |
| 3 | 1 nF C0G capacitor | Direct CV low-pass |
| 4 | 10 nF C0G capacitor | CV output reconstruction |
| 6 | 100 nF film capacitor | Supply decoupling |
| 2 | 470 uF 25 V electrolytic capacitor | 12 V input and main rail bulk |
| 1 | 330 uF 10 V electrolytic capacitor | Buck output bulk |
| 1 | 470 uF 10 V electrolytic capacitor | Analog rail reservoir |

## External Or Omitted From This BOM

These parts are outside the TME electronics list above and should be handled separately.

- Raspberry Pi Zero 2 W
- Waveshare ADS1256 ADC board
- Manual pots, motorized faders, and NeoSlider module
- Panel jacks, barrel jack, GPIO header, and switches
- Enclosure, standoffs, screws, spacers, labels, knobs, and other mechanical hardware

## Notes

- I did not keep AD780ANZ in the TME BOM because TME shows it as hardly available with 0 stock.
- LM4040AIZ-2.5/NOPB is the stocked TME replacement I would use for the 2.5 V reference rail.
- TL431ACP is still not the right direct reference substitute for this circuit.
- The mux is the only part here that may force a footprint decision if you want to stay purely through-hole.