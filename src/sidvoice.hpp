#pragma once
// One SID6581 voice: a complete emulated MOS 6581 chip (reSID) plus a small
// C64-style "player" that runs at the PAL frame rate (50 Hz) and animates the
// chip registers the way C64 music routines do (wavetables / arpeggios, pulse
// width sweeps, filter sweeps, vibrato, drum pitch drops).
//
// This file does not depend on VCV Rack so the engine can be tested on its own.

#include <cstdint>
#include <string>
#include <vector>
#include "sid.h"

namespace sidv {

// Control register waveform bits
enum : uint8_t {
	TRI = 0x10,
	SAW = 0x20,
	PULSE = 0x40,
	NOISE = 0x80,
};

// Filter mode bits (register $18). LP | HP gives a notch.
enum : uint8_t {
	LP = 0x10,
	BP = 0x20,
	HP = 0x40,
};

// PAL C64 system clock
const double SID_CLOCK = 985248.0;
const float FRAME_RATE = 50.f;

struct Osc {
	// Audible. Oscillator 1 is always on; 2 and 3 are optional layers.
	// Oscillator 3 always runs at its pitch (it is the ring / sync source for
	// oscillator 1) even when it is not on.
	bool on = false;
	uint8_t wave = PULSE;
	bool ring = false; // ring-modulated by the previous oscillator (1<-3, 2<-1, 3<-2)
	bool sync = false; // hard-synced to the previous oscillator
	bool filter = true; // routed through the filter
	float semis = 0.f; // transpose; the fraction is a detune
	int pw = 2048; // 0..4095
	// Pulse width sweep: units per frame, bouncing between pwMin and pwMax
	int pwSpeed = 0;
	int pwMin = 0x200;
	int pwMax = 0xE00;
	// 4-bit SID envelope rates and sustain level
	uint8_t a = 0, d = 9, s = 0, r = 9;
};

// One wavetable row, played one per `wtSpeed` frames from the gate.
struct Step {
	int wave; // control-register waveform bits for this row, or -1 to keep the oscillator's own
	int note; // semitones above the played note (or above C4 when abs is set)
	bool abs;
};

struct Patch {
	std::string name;
	std::string category;
	Osc osc[3];

	uint8_t filterMode = LP;
	int cutoff = 1024; // 11 bit
	int res = 0; // 4 bit
	// Filter sweep from the gate: units per frame, limited to cutMin..cutMax;
	// with cutLoop it bounces between them instead of stopping.
	int cutSpeed = 0;
	int cutMin = 0;
	int cutMax = 2047;
	bool cutLoop = false;
	int volume = 15;

	// Frame-stepped triangle vibrato
	float vibDepth = 0.f; // semitones
	float vibRate = 5.f; // Hz
	int vibDelay = 0; // frames

	// Pitch envelope: starts `pitchEnv` semitones away and falls back over
	// `pitchEnvFrames` frames. Applies to the oscillators in pitchEnvMask.
	float pitchEnv = 0.f;
	int pitchEnvFrames = 0;
	uint8_t pitchEnvMask = 7;

	std::vector<Step> wt;
	int wtLoop = -1; // row to jump to after the last; -1 holds the last row
	int wtSpeed = 1;
	uint8_t wtMask = 1; // oscillators the wavetable drives
};

// Factory bank: 64 patches in 8 categories of 8 (see patches.cpp)
const std::vector<Patch>& factoryPatches();

struct Mods {
	float pw = 0.f; // pulse width offset, register units
	float cutoff = 0.f; // cutoff offset, register units
	float res = 0.f; // resonance offset, register units
	bool drone = false; // holds sustain at maximum (used when no gate is patched)
};

struct SidVoice {
	SidVoice();

	void setSampleRate(float sampleRate, bool highQuality);
	void setChipModel(bool mos8580);
	void setFilterBias(float volts);
	// Selects the patch; the player restarts on the next gate.
	void setPatch(const Patch* p);
	// Drops the gate for a moment, so a held note restarts its envelope.
	void retrigger();
	void reset();

	// pitch: 1V/oct relative to C4. Returns the chip output, about +/-1.
	float process(float pitch, bool gate, const Mods& mods);

private:
	void startNote();
	void tick();
	void writeReg(int addr, int value);

	reSID::SID sid;
	const Patch* patch = nullptr;
	int regs[0x19];

	float sampleRate = 44100.f;
	float samplesPerFrame = 882.f;
	float frameCountdown = 0.f;
	bool gate = false;
	int killGate = 0;

	// Player state
	int frame = 0;
	int wtIdx = 0;
	int wtTimer = 0;
	int wtWave = -1;
	int wtNote = 0;
	bool wtAbs = false;
	int pw[3] = {};
	int pwVel[3] = {};
	int cut = 0;
	int cutVel = 0;
	float vibPhase = 0.f;
	float vib = 0.f;
	float penv = 0.f;

	// Idle detection: a silent voice with its gate down stops clocking the chip
	int quiet = 0;
	bool sleeping = false;
};

} // namespace sidv
