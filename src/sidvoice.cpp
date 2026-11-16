#include "sidvoice.hpp"
#include <algorithm>
#include <cmath>

namespace sidv {

namespace {

inline int clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// 16-bit SID frequency register for a pitch in volts above C4
inline int freqReg(float volts) {
	const float c4 = 261.6256f;
	const float scale = (float) (16777216.0 / SID_CLOCK);
	float hz = c4 * std::exp2(std::min(volts, 4.f));
	return clampi((int) (hz * scale + 0.5f), 0, 0xFFFF);
}

} // namespace


SidVoice::SidVoice() {
	sid.set_chip_model(reSID::MOS6581);
	sid.enable_filter(true);
	sid.enable_external_filter(true);
	setSampleRate(44100.f, true);
	reset();
}

void SidVoice::setSampleRate(float sr, bool highQuality) {
	sampleRate = sr;
	samplesPerFrame = sr / FRAME_RATE;
	bool ok = false;
	if (highQuality)
		ok = sid.set_sampling_parameters(SID_CLOCK, reSID::SAMPLE_RESAMPLE, sr);
	if (!ok)
		sid.set_sampling_parameters(SID_CLOCK, reSID::SAMPLE_INTERPOLATE, sr);
}

void SidVoice::setChipModel(bool mos8580) {
	sid.set_chip_model(mos8580 ? reSID::MOS8580 : reSID::MOS6581);
}

void SidVoice::setFilterBias(float volts) {
	sid.adjust_filter_bias(volts);
}

void SidVoice::setPatch(const Patch* p) {
	patch = p;
	// Reload the player's starting state so a held drone picks up the new patch
	if (patch)
		startNote();
}

void SidVoice::retrigger() {
	killGate = 1;
}

void SidVoice::reset() {
	sid.reset();
	for (int& r : regs)
		r = -1;
	gate = false;
	killGate = 0;
	frameCountdown = 0.f;
	quiet = 0;
	sleeping = false;
}

void SidVoice::writeReg(int addr, int value) {
	if (regs[addr] != value) {
		regs[addr] = value;
		sid.write(addr, value);
	}
}

void SidVoice::startNote() {
	frame = 0;
	wtIdx = 0;
	wtTimer = 0;
	wtWave = -1;
	wtNote = 0;
	wtAbs = false;
	for (int i = 0; i < 3; i++) {
		pw[i] = patch->osc[i].pw;
		pwVel[i] = patch->osc[i].pwSpeed;
	}
	cut = patch->cutoff;
	cutVel = patch->cutSpeed;
	vibPhase = 0.f;
	if (!patch->wt.empty()) {
		const Step& s = patch->wt[0];
		wtWave = s.wave;
		wtNote = s.note;
		wtAbs = s.abs;
	}
	vib = 0.f;
	penv = patch->pitchEnvFrames > 0 ? patch->pitchEnv : 0.f;
	frameCountdown = samplesPerFrame;
}

// One 50 Hz player frame
void SidVoice::tick() {
	const Patch& p = *patch;
	frame++;

	if (!p.wt.empty() && ++wtTimer >= std::max(1, p.wtSpeed)) {
		wtTimer = 0;
		int n = (int) p.wt.size();
		if (++wtIdx >= n)
			wtIdx = (p.wtLoop >= 0 && p.wtLoop < n) ? p.wtLoop : n - 1;
		const Step& s = p.wt[wtIdx];
		wtWave = s.wave;
		wtNote = s.note;
		wtAbs = s.abs;
	}

	for (int i = 0; i < 3; i++) {
		const Osc& o = p.osc[i];
		if (!pwVel[i])
			continue;
		pw[i] += pwVel[i];
		if (pw[i] >= o.pwMax) {
			pw[i] = o.pwMax;
			pwVel[i] = -std::abs(pwVel[i]);
		}
		else if (pw[i] <= o.pwMin) {
			pw[i] = o.pwMin;
			pwVel[i] = std::abs(pwVel[i]);
		}
	}

	if (cutVel) {
		cut += cutVel;
		if (cut >= p.cutMax) {
			cut = p.cutMax;
			cutVel = p.cutLoop ? -std::abs(cutVel) : 0;
		}
		else if (cut <= p.cutMin) {
			cut = p.cutMin;
			cutVel = p.cutLoop ? std::abs(cutVel) : 0;
		}
	}

	if (p.vibDepth > 0.f && frame >= p.vibDelay) {
		vibPhase += p.vibRate / FRAME_RATE;
		vibPhase -= std::floor(vibPhase);
		vib = p.vibDepth * (1.f - 4.f * std::fabs(vibPhase - 0.5f));
	}
	else {
		vib = 0.f;
	}

	if (frame < p.pitchEnvFrames) {
		float t = 1.f - (float) frame / p.pitchEnvFrames;
		penv = p.pitchEnv * t * t;
	}
	else {
		penv = 0.f;
	}
}

float SidVoice::process(float pitch, bool g, const Mods& mods) {
	if (!patch)
		return 0.f;

	if (killGate > 0) {
		killGate--;
		g = false;
	}
	if (sleeping) {
		if (!g)
			return 0.f;
		sleeping = false;
		quiet = 0;
	}
	if (g && !gate)
		startNote();
	gate = g;

	frameCountdown -= 1.f;
	if (frameCountdown <= 0.f) {
		tick();
		frameCountdown += samplesPerFrame;
	}

	const Patch& p = *patch;
	const bool hasWt = !p.wt.empty();
	const int pwOffset = (int) std::lround(mods.pw);
	int filt = 0;

	for (int i = 0; i < 3; i++) {
		const Osc& o = p.osc[i];
		const int base = i * 7;
		const bool wtOn = hasWt && ((p.wtMask >> i) & 1);
		float semis = o.semis + vib + (((p.pitchEnvMask >> i) & 1) ? penv : 0.f);
		float volts;
		if (wtOn)
			volts = (wtAbs ? 0.f : pitch) + (wtNote + semis) / 12.f;
		else
			volts = pitch + semis / 12.f;

		// Oscillator 1's sync / ring source (3) must run even when silent
		if (o.on || i == 2) {
			int f = freqReg(volts);
			writeReg(base + 0, f & 0xFF);
			writeReg(base + 1, f >> 8);
		}
		if (!o.on) {
			writeReg(base + 4, 0);
			continue;
		}

		int w = clampi(pw[i] + pwOffset, 0, 4095);
		writeReg(base + 2, w & 0xFF);
		writeReg(base + 3, w >> 8);
		writeReg(base + 5, (o.a << 4) | o.d);
		writeReg(base + 6, ((mods.drone ? 15 : o.s) << 4) | o.r);

		int wave = (wtOn && wtWave >= 0) ? wtWave : o.wave;
		int ctrl = (wave & 0xF0) | (o.ring ? 0x04 : 0) | (o.sync ? 0x02 : 0) | (g ? 0x01 : 0);
		writeReg(base + 4, ctrl);
		if (o.filter)
			filt |= 1 << i;
	}

	int c = clampi(cut + (int) std::lround(mods.cutoff), 0, 2047);
	writeReg(0x15, c & 7);
	writeReg(0x16, c >> 3);
	int r = clampi(p.res + (int) std::lround(mods.res), 0, 15);
	writeReg(0x17, (r << 4) | filt);
	// Oscillator 3 is muted with 3OFF when it only serves as a modulator
	writeReg(0x18, p.filterMode | (p.osc[2].on ? 0 : 0x80) | clampi(p.volume, 0, 15));

	short buf = 0;
	reSID::cycle_count delta = 1 << 20;
	sid.clock(delta, &buf, 1);
	float out = buf * (1.f / 32768.f);

	// Sleep after half a second of silence with the gate down
	if (!g && std::fabs(out) < 3e-4f) {
		if (++quiet > (int) (sampleRate * 0.5f))
			sleeping = true;
	}
	else {
		quiet = 0;
	}
	return out;
}

} // namespace sidv
