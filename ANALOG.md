# Analog Front End

This document covers the Pi-only analog front end used on the AARCH64 build.

## Overview

The analog path is split into three parts:

- Three direct CV inputs on the Waveshare board ADC channels `AD1`, `AD2`, and `AD3`.
- A 16-channel potentiometer bank on the Waveshare board ADC input `AD0` using a `CD74HC4067`.
- Three separate gate inputs on free Pi GPIO pins for note/gate/trigger events.

The runtime support is AARCH64-only and prints sampled CV values and gate states to the console.

## Detailed Proposed Schematic

The preferred front-end is drawn in [docs/analog-front-end-detailed.svg](docs/analog-front-end-detailed.svg). It shows the proposed element values for the three direct CV inputs on `AD1`-`AD3`, the `AD0` pot mux, and the gate inputs in one board-level diagram.

The short version is:

- Direct CV attenuation for 0-10 V sources: `100k` / `100k` divider.
- Direct CV series protection: `1k`.
- Direct CV clamp: `BAT54S` dual Schottky to `0 V` and `3V3`.
- Direct CV low-pass filter: `1 nF` to ground.
- Optional bipolar CV stage: `MCP6002` rail-to-rail op-amp with `100k` / `100k` mid-rail bias.
- Pot mux series protection: `1k`.
- Pot mux clamp: `BAT54S` dual Schottky to `0 V` and `3V3`.
- Gate series resistor: `10k`.
- Gate pull-down: `100k`.
- Gate clamp: `BAT54S` to `0 V` and `3V3`.
- Gate buffer: `74LVC14` Schmitt trigger at `3.3 V`.

## Direct CV Inputs On `AD1`-`AD3`

The three direct CV inputs are wired to the ADC channels `AD1`, `AD2`, and `AD3`. They do not go through the `CD74HC4067`; only the pot bank uses `AD0`.

Each CV channel assumes a 3-position mode switch in front of the conditioning block:

- Position 1: direct `0-5 V` input
- Position 2: `0-10 V` attenuated input
- Position 3: bipolar input shifted into the `0 V` to `5 V` range

### Pin Map

| CV channel | ADC input | Notes |
|---|---|---|
| `CV0` | `AD1` | direct analog input |
| `CV1` | `AD2` | direct analog input |
| `CV2` | `AD3` | direct analog input |

### Power And Wiring

| CV pin | Connect to |
|---|---|
| `CV0` / `AD1` | direct conditioned CV source |
| `CV1` / `AD2` | direct conditioned CV source |
| `CV2` / `AD3` | direct conditioned CV source |

### Direct CV Circuit

Each direct CV input uses the same element set:

- `Jx` input jack
- `R1` `100k` / `100k` attenuator for `0-10 V` sources
- `R2` `1k` series protection resistor
- `D1` `BAT54S` clamp to `0 V` and `3V3`
- `C1` `1 nF` to ground
- optional `U1` `MCP6002` if bipolar CV needs mid-rail shifting

### CV Input Rules

- Keep all CV sources referenced to the same ground as the Waveshare board.
- Do not feed negative voltage into the ADC input path.
- Do not exceed the ADC input range. If a source is above 5 V, attenuate it first.
- For bipolar CV, shift it into the 0 V to 5 V range before `AD1`-`AD3`.

### Direct CV Conditioning Block Diagram

![Direct CV conditioning block diagram](docs/analog-cv-input.svg)

## Pot Mux On `AD0`

The mux common pin goes to `AD0`. The Pi drives the mux select lines and the ADC samples whichever pot channel is selected. The mux is used only for potentiometers, not for the direct CV inputs.

### Pin Map

| Function | Pi BCM | Physical pin |
|---|---|---|
| `S0` | 5 | 29 |
| `S1` | 6 | 31 |
| `S2` | 13 | 33 |
| `S3` | 26 | 37 |

### Power And Wiring

| Mux pin | Connect to |
|---|---|
| `VCC` | Waveshare board `3V3` |
| `GND` | Waveshare board `GND` |
| `COM` / `SIG` | `AD0` |
| `S0` | Pi `GPIO5` |
| `S1` | Pi `GPIO6` |
| `S2` | Pi `GPIO13` |
| `S3` | Pi `GPIO26` |
| `EN` | `GND` |
| `VEE` | `GND` if exposed |

Wire each potentiometer with one outer leg to `3V3`, the other outer leg to `GND`, and the wiper to one of the mux channels `C0` through `C15`.

### Pot Mux Rules

- Use only potentiometers on the mux channels.
- Keep the pot grounds common with the Waveshare board.
- Each mux channel should stay inside the ADC range after the pot wiring and any local series protection.

### Pot Mux Schematic

The pot mux is shown in the board-level schematic above. It is the `AD0` section of [docs/analog-front-end-detailed.svg](docs/analog-front-end-detailed.svg).

## Gate Inputs

Three gate inputs are read as digital GPIO states. These are intended for on/off modulation sources such as synth gates, sequencer gates, envelopes, and triggers.

### Pin Map

| Gate | Pi BCM | Physical pin |
|---|---|---|
| `GATE0` | 16 | 36 |
| `GATE1` | 19 | 35 |
| `GATE2` | 20 | 38 |

### Power And Wiring

| Gate front-end pin | Connect to |
|---|---|---|
| `OUT0` | Pi `GPIO16` |
| `OUT1` | Pi `GPIO19` |
| `OUT2` | Pi `GPIO20` |
| `VCC` | Waveshare board `3V3` or another regulated `3.3 V` rail |
| `GND` | Waveshare board `GND` |

### Gate Input Rules

- Convert each incoming gate to clean 3.3 V logic before it reaches the Pi GPIO.
- Keep a common ground between the front-end board, the Pi, and any external synth or sequencer.
- Do not connect 5 V gate signals directly to the GPIO pins.
- If the source can swing negative or exceed 3.3 V, use a resistor, clamp, comparator, or Schmitt trigger stage.

Suggested front-end channel:

- Input resistor: 10 k to 100 k
- Clamp: to 0 V and 3.3 V
- Optional pull-down: 100 k to GND
- Optional Schmitt stage for cleaner edges

### Gate Input Schematic

![Gate input schematic](docs/analog-gate-input.svg)

## Runtime Output

The monitor prints the analog and gate state to the console.

- CV mux channels appear as `[ads1256] mux AD7 = ...`
- Gate states appear as `[ads1256] gates G0=HIGH G1=LOW G2=HIGH`

The gate polling interval can be adjusted with `COCKSCREEN_ADS1256_GATE_POLL_MS`. The mux scan count can be forced back to a single direct ADC read with `COCKSCREEN_ADS1256_MUX_CHANNELS=1`.
