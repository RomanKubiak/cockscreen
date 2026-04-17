# TME Electronics BOM

This is the TME-focused sourcing list for the current project.

Mechanical and panel-only items are intentionally omitted here: enclosure, standoffs, screws, panel hardware, knobs, manual pots, motorized faders, NeoSlider module, L9110 motor-driver module, wire, perfboard, jacks, barrel jack, GPIO header, and switches.

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
| 1 | Gate inverter | SN74HCT14N | exact, in stock | DIP14 Schmitt-trigger inverter; 46 in TME stock |
| 1 | Button expander | MCP23017-E/SP | exact, check stock | DIP28 I2C GPIO expander for eight control buttons on `GPA0`-`GPA7` |
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
- Manual pots, motorized faders, NeoSlider module, and panel push buttons
- Panel jacks, barrel jack, GPIO header, and switches
- Enclosure, standoffs, screws, spacers, labels, knobs, and other mechanical hardware

## Optional Starter DIY Add-On BOM

This is a practical through-hole add-on list for experimenting with Eurorack-style CV/gate utilities and simple analog audio circuits. It is intentionally biased toward reusable parts instead of one-off project-specific ICs.

The target is roughly 200 PLN at TME, depending on current stock and package selection.

| Qty | Part | Use | Notes |
|---|---|---|---|
| 2 | TL072CP | General-purpose dual op amp | Good baseline for filters, mixers, buffers, and CV processing |
| 2 | NE5532P | Lower-noise dual op amp | Useful for audio mixers, filters, and line-level stages |
| 2 | LM358N | Utility dual op amp | Handy for envelope followers, comparators, and low-voltage helpers |
| 2 | LM393N | Dual comparator | Strong choice for gate detection, thresholding, and simple logic |
| 2 | CD40106BE | Schmitt trigger inverter | Great for pulse shaping, square-wave logic, and gates |
| 2 | CD4046BE | PLL / VCO helper | Useful for clock recovery, simple FM experiments, and modulation |
| 1 | LM13700N | OTA for filters/VCA experiments | One of the most flexible through-hole analog-synth ICs |
| 1 | NE556N | Dual timer | Good for LFOs, clocks, and modulation sources |
| 2 | TL074CN | Quad op amp | Extra headroom for more complex filters and mixers |
| 2 | 78L05 | 5 V regulator | Handy for submodules and test fixtures |
| 2 | 79L05 | -5 V regulator | Useful for bipolar signal experiments and test rigs |
| 10 | 1N4148 | Signal diode | Saturation, protection, rectification, and switching |
| 10 | 1N5819 | Schottky diode | Lower-drop protection and clamping |
| 10 | BC547B | General-purpose NPN transistor | Gate drivers, switches, current sources, and simple logic |
| 10 | BC557B | General-purpose PNP transistor | Complementary stages and utility analog switching |
| 5 | 100 nF film capacitor | Decoupling and timing | Good default value for analog boards |
| 5 | 10 nF C0G capacitor | Timing and filters | Stable for RC networks |
| 5 | 1 nF C0G capacitor | Audio/CV filtering | Good for tighter filter time constants |
| 5 | 10 uF electrolytic capacitor | Coupling and filtering | General-purpose helper stock |
| 5 | 100 uF electrolytic capacitor | Power and modulation rails | Good bulk and smoothing stock |
| 20 | 1 k resistor | General-purpose | Series resistors, LED limits, and logic helpers |
| 20 | 10 k resistor | General-purpose | The default value for synth DIY |
| 20 | 100 k resistor | General-purpose | Biasing, pull-ups, and CV ranges |
| 10 | 47 k resistor | Mixing and filter networks | Very common in analog audio designs |
| 10 | 1 M resistor | Timing and bias | Useful for LFOs, envelopes, and high-impedance inputs |
| 4 | 100 k trimmer | Calibration | Offsets, thresholds, and gain trimming |
| 4 | 10 k trimmer | Calibration | Fine adjust for filters and control ranges |
| 2 | 9-pin DIP socket | Test and swap convenience | Good for trying different op amps without soldering them directly |

Suggested first builds from this pile:

- Buffered CV input and gate conditioner
- Simple active mixer / attenuverter
- Two-pole low-pass or band-pass filter
- Envelope follower / comparator gate extractor
- LFO or clock source
- Basic utility VCA or OTA filter experiment with LM13700N

I would not try to force a full analog delay or chorus into this first order. Those are possible later, but they usually need more specialized parts, more board space, and tighter gain/clock choices than a starter stockpile.

## Notes

- I did not keep AD780ANZ in the TME BOM because TME shows it as hardly available with 0 stock.
- LM4040AIZ-2.5/NOPB is the stocked TME replacement I would use for the 2.5 V reference rail.
- TL431ACP is still not the right direct reference substitute for this circuit.
- The mux is the only part here that may force a footprint decision if you want to stay purely through-hole.