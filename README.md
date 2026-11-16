# SID6581

A VCV Rack 2 plugin containing one module, **SID6581**: a four-voice VCO built around an emulated
MOS 6581, the SID sound chip of the Commodore 64. It is 30 HP.

Each voice is a complete SID chip (three oscillators, envelope generators and the 6581's
multimode filter), emulated cycle by cycle with [reSID](https://github.com/libsidplayfp/resid). The
voices are independent: each has its own patch, filter, controls, jacks and output.

## Panel

Each voice column, top to bottom:

| Control | What it does |
|---|---|
| Patch display | The patch number, category and name. The arrows step through the 64 factory patches; clicking the name opens the patch browser. Patch changes can be undone with Ctrl+Z. |
| OCTAVE / TUNE | Transpose in octaves (-4 to +3) and semitones (±12). |
| PW | Offsets the patch's pulse width (±50% duty). |
| CUTOFF / RES | Offset the patch's filter cutoff and resonance. |
| LEVEL | Output level. |
| V/OCT, GATE | Pitch (0 V = C4) and gate. The gate drives the SID's envelopes and restarts the patch's sequence (see below). |
| PW, CUTOFF inputs | CV for the two offsets: ±5 V covers the full knob range. |
| OUT | The voice's audio, about ±5 V. The green light shows level; the yellow light shows drone mode. |

The **CHIP** column has the 6581 filter **BIAS** knob, plus the **POLY** (voices 1–4 as four
channels) and **MIX** outputs. Real 6581s varied widely from chip to chip: some had dark,
closed-sounding filters and others bright ones. BIAS reproduces that spread. The patches are voiced
with BIAS at its default (0.5 V).

### Normalling and polyphony

An unpatched V/OCT or GATE jack follows the nearest patched jack to its left:

- **Polyphonic cable into voice 1:** channels 1–4 play voices 1–4, so the module works as a
  four-voice polysynth with a different patch on each voice.
- **Mono cable into voice 1:** all four voices play the same note, which stacks four patches.
- **Drone:** a voice with no gate source at all drones like a plain VCO. Its envelope sustain is
  held at maximum and the yellow light is on. Changing the patch restarts the note so you hear the
  attack.

## Patches

There are 64 factory patches, eight in each of these categories:

| Category | Patches |
|---|---|
| Bass | Pulse Bass, Saw Bass, Sync Growl, Wobble, Sub Triangle, Fat Detune, Pluck Bass, Slap Bass |
| Lead | Square Lead, PWM Lead, Saw Lead, Chip Flute, Hollow, Nasal Lead, Octave Lead, Power Lead |
| Sync & Ring | Sync Sweep, Sync Lead, Ring Bell, Ring Gong, Metal Clang, Ring Chime, Sync Brass, Robot Ring |
| Keys | Harpsichord, Electric Piano, Clavi, Marimba, Pizzicato, Music Box, Organ, Koto |
| Pad | Warm Pad, PWM Strings, Glass Pad, Choir, Dark Drone, Sweep Pad, Notch Pad, Space Strings |
| Arp | Major Arp, Minor Arp, Maj7 Arp, Octave Arp, Sus4 Arp, Diminished, Power Chord, Trill |
| Drums | Kick, Snare, Closed Hat, Open Hat, Tom (pitched), Clap, Laser Tom, Cowbell |
| FX | Laser, Explosion, Siren, Wind, Bubbles, Alarm, Teleport, Coin |

A patch sets up the whole chip:

- Oscillator 1 is the voice.
- Oscillator 2 is an optional layer.
- Oscillator 3 is either a third layer or the silent sync and ring-mod source for oscillator 1.

Many classic SID sounds come from the C64 music routine rather than the chip itself. Each voice
therefore also runs a small "player" at the PAL frame rate (50 Hz) that animates the chip's
registers, as C64 music drivers do:

- wavetables: drum hits that start on noise and switch to pulse, and arpeggios at 50 Hz
- pulse-width sweeps
- filter sweeps
- delayed vibrato
- pitch drops

The player restarts on each gate.

Most drum patches play at a fixed pitch whatever the V/OCT input; Tom (pitched) follows it.

The bank is defined in [src/patches.cpp](src/patches.cpp).

## Context menu

- **Voice 1–4 patch:** the patch browser for each voice.
- **Chip model:** MOS 6581 (default) or MOS 8580. The 8580 has a cleaner, more linear filter; the
  patches are voiced for the 6581.
- **Resampling:** *High quality* (reSID's FIR resampler) or *Low CPU* (interpolation).

## CPU

Each voice clocks a full SID at about 1 MHz, so four active voices use roughly 20% of one CPU
core at 48 kHz. Two things keep the cost down:

- A voice whose gate is down stops clocking its chip after half a second of silence.
- A voice is not processed at all when its OUT jack is unpatched and neither MIX nor POLY is
  patched.

## Building

Requires the [VCV Rack 2 SDK](https://vcvrack.com/manual/Building#Building-Rack-plugins). On
Windows, build from an MSYS2 MinGW 64-bit shell. The Makefile expects the SDK at `../Rack-SDK`
(override with `make RACK_DIR=/path/to/Rack-SDK`):

```sh
make -j8      # builds plugin.dll / .so / .dylib
make dist     # packages dist/SID6581-2.0.0-<arch>.vcvplugin
make install RACK_USER_DIR="C:/Users/utmos/AppData/Local/Rack2"  # copies the package into your Rack user folder
make test     # renders every factory patch to build/test/*.wav (no Rack needed)
```

reSID is vendored in `dep/resid` (cloned from https://github.com/libsidplayfp/resid). The build
generates the headers that reSID's autotools setup normally produces (`siddefs.h` and the
`wave*.h` waveform tables); this needs `sed` and `perl`, both of which MSYS2 provides.

If g++ fails with "Cannot create temporary file in C:\WINDOWS\", pass a temp folder to make:
`make TMP=C:/path/to/tmp TEMP=C:/path/to/tmp`.

## Implementation notes

- **Engine:** [src/sidvoice.cpp](src/sidvoice.cpp) wraps one `reSID::SID` per voice. Register
  writes are cached, so the chip only sees a write when a value changes. Each Rack sample clocks the
  chip for one output sample (about 20 SID cycles at 48 kHz).
- **Pitch:** the 16-bit SID frequency register is recomputed every sample, so V/OCT and FM are
  continuous. As on the hardware, the register's resolution limits low notes to a few cents of
  accuracy, and the highest note is about 3.9 kHz.
- **Engine tests:** [test/render.cpp](test/render.cpp) has three modes. `render calib` and
  `render bias` measure the filter cutoff curve; `render all` renders the bank and prints levels.

## License

The plugin is GPL-3.0-or-later. reSID is © Dag Lem and contributors, under the GPL-2.0-or-later.
