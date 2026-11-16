// SID6581 factory bank: 64 patches, 8 categories of 8.
//
// Each patch sets up one 6581 chip the way a C64 music routine would:
// oscillator 1 is the voice, oscillator 2 is an optional layer, and oscillator 3
// is either a third layer or, when it is not switched on, the silent sync /
// ring-mod source for oscillator 1.
//
// Filter cutoffs are voiced for the default 6581 filter bias (CHIP knob at
// noon); on that setting $000-$0C0 is nearly closed, $100-$300 is the
// "speaking" range and above $500 is close to open.
//
// SID envelope rates (index 0-15):
//   attack   2ms 8ms 16ms 24ms 38ms 56ms 68ms 80ms 100ms 250ms 500ms 800ms 1s 3s 5s 8s
//   dec/rel  three times the attack time (6ms .. 24s)

#include "sidvoice.hpp"
#include <initializer_list>

namespace sidv {

namespace {

// Chained patch builder. Oscillator calls apply to the oscillator picked with
// o1() / o2() / o3() (o1 is current to begin with).
struct B {
	Patch p;
	int cur = 0;

	B(const char* name, const char* cat) {
		p.name = name;
		p.category = cat;
		p.osc[0].on = true;
	}

	B& o1() { cur = 0; return *this; }
	B& o2() { cur = 1; p.osc[1].on = true; return *this; }
	B& o3() { cur = 2; p.osc[2].on = true; return *this; }
	// Oscillator 3 as a silent modulator for ring / sync, at `semis` from the note
	B& mod3(float semis) { p.osc[2].on = false; p.osc[2].semis = semis; return *this; }

	Osc& o() { return p.osc[cur]; }
	B& wave(uint8_t w) { o().wave = w; return *this; }
	B& adsr(int a, int d, int s, int r) { o().a = a; o().d = d; o().s = s; o().r = r; return *this; }
	B& semi(float s) { o().semis = s; return *this; }
	B& pw(int v) { o().pw = v; return *this; }
	B& pwm(int speed, int lo, int hi) { o().pwSpeed = speed; o().pwMin = lo; o().pwMax = hi; return *this; }
	B& ring() { o().ring = true; return *this; }
	B& sync() { o().sync = true; return *this; }
	B& dry() { o().filter = false; return *this; }
	// Every oscillator bypasses the filter
	B& nofilter() { for (Osc& x : p.osc) x.filter = false; return *this; }

	B& filter(uint8_t mode, int cutoff, int res) { p.filterMode = mode; p.cutoff = cutoff; p.res = res; return *this; }
	B& sweep(int speed, int lo, int hi, bool loop = false) { p.cutSpeed = speed; p.cutMin = lo; p.cutMax = hi; p.cutLoop = loop; return *this; }
	B& vib(float depth, float rate, int delay) { p.vibDepth = depth; p.vibRate = rate; p.vibDelay = delay; return *this; }
	B& penv(float semis, int frames, uint8_t mask = 7) { p.pitchEnv = semis; p.pitchEnvFrames = frames; p.pitchEnvMask = mask; return *this; }
	B& wt(std::initializer_list<Step> steps, int loop = -1, int speed = 1, uint8_t mask = 1) {
		p.wt = steps; p.wtLoop = loop; p.wtSpeed = speed; p.wtMask = mask; return *this;
	}
	// Arpeggio: semitone offsets looped one per `speed` frames
	B& arp(std::initializer_list<int> notes, int speed = 1, uint8_t mask = 1) {
		p.wt.clear();
		for (int n : notes)
			p.wt.push_back({-1, n, false});
		p.wtLoop = 0; p.wtSpeed = speed; p.wtMask = mask; return *this;
	}
	B& vol(int v) { p.volume = v; return *this; }
};

const int K = -1; // wavetable: keep the oscillator's waveform

std::vector<Patch> build() {
	std::vector<Patch> v;
	auto add = [&](B& b) {v.push_back(b.p);};

	// ---- Bass ---------------------------------------------------------------
	{ B b("Pulse Bass", "Bass"); b.wave(PULSE).pw(0x400).pwm(24, 0x200, 0x700).adsr(0, 9, 8, 6)
		.filter(LP, 0x500, 10).sweep(-14, 0x180, 0x500); add(b); }
	{ B b("Saw Bass", "Bass"); b.wave(SAW).adsr(0, 8, 10, 4).filter(LP, 0x600, 8).sweep(-20, 0x1C0, 0x600); add(b); }
	{ B b("Sync Growl", "Bass"); b.wave(SAW).sync().semi(12).adsr(0, 9, 9, 5).mod3(0)
		.penv(12, 20, 1).filter(LP, 0x400, 6); add(b); }
	{ B b("Wobble", "Bass"); b.wave(PULSE).pw(0x600).adsr(0, 0, 15, 4)
		.filter(LP, 0x150, 12).sweep(26, 0x150, 0x600, true); add(b); }
	{ B b("Sub Triangle", "Bass"); b.wave(TRI).adsr(0, 9, 13, 5).o2().wave(PULSE).pw(0x200).semi(12).adsr(0, 6, 5, 5).nofilter(); add(b); }
	{ B b("Fat Detune", "Bass"); b.wave(SAW).adsr(0, 9, 11, 5).o2().wave(SAW).semi(0.12f).adsr(0, 9, 11, 5)
		.o3().wave(PULSE).pw(0x800).semi(-12).adsr(0, 9, 11, 5).filter(LP, 0x380, 4); add(b); }
	{ B b("Pluck Bass", "Bass"); b.wave(PULSE).pw(0x800).adsr(0, 7, 0, 7).filter(LP, 0x700, 9).sweep(-60, 0x150, 0x700); add(b); }
	{ B b("Slap Bass", "Bass"); b.wave(PULSE).pw(0x300).adsr(0, 9, 4, 5)
		.wt({{SAW, 12, false}, {K, 0, false}}).filter(LP, 0x600, 6).sweep(-40, 0x200, 0x600); add(b); }

	// ---- Lead ---------------------------------------------------------------
	{ B b("Square Lead", "Lead"); b.wave(PULSE).pw(0x800).adsr(0, 9, 10, 6).vib(0.3f, 5.5f, 15).nofilter(); add(b); }
	{ B b("PWM Lead", "Lead"); b.wave(PULSE).pw(0x400).pwm(40, 0x200, 0xE00).adsr(0, 9, 11, 6).vib(0.25f, 5.f, 20).nofilter(); add(b); }
	{ B b("Saw Lead", "Lead"); b.wave(SAW).adsr(0, 9, 11, 6).o2().wave(SAW).semi(0.1f).adsr(0, 9, 11, 6)
		.vib(0.2f, 5.5f, 20).filter(LP, 0x600, 4); add(b); }
	{ B b("Chip Flute", "Lead"); b.wave(TRI).adsr(3, 9, 12, 6).vib(0.4f, 6.f, 10).nofilter(); add(b); }
	{ B b("Hollow", "Lead"); b.wave(TRI | SAW).adsr(0, 8, 12, 6).o2().wave(TRI).semi(12).adsr(0, 8, 8, 6)
		.vib(0.25f, 5.f, 18).nofilter(); add(b); }
	{ B b("Nasal Lead", "Lead"); b.wave(PULSE).pw(0x200).adsr(1, 9, 11, 6).filter(LP | BP, 0x300, 13).vib(0.3f, 5.5f, 15); add(b); }
	{ B b("Octave Lead", "Lead"); b.wave(PULSE).pw(0x600).pwm(30, 0x400, 0xC00).adsr(0, 9, 11, 6).arp({0, 12}).nofilter(); add(b); }
	{ B b("Power Lead", "Lead"); b.wave(PULSE).pw(0x800).adsr(0, 9, 11, 6).o2().wave(PULSE).pw(0x600).semi(7).adsr(0, 9, 10, 6)
		.o3().wave(SAW).semi(-12).adsr(0, 9, 10, 6).filter(LP, 0x700, 5).vib(0.2f, 5.f, 25); add(b); }

	// ---- Sync & Ring --------------------------------------------------------
	{ B b("Sync Sweep", "Sync & Ring"); b.wave(SAW).sync().adsr(0, 10, 10, 7).mod3(0).penv(24, 60, 1).nofilter(); add(b); }
	{ B b("Sync Lead", "Sync & Ring"); b.wave(PULSE).pw(0x800).sync().semi(19).adsr(0, 9, 11, 6).mod3(0)
		.o2().wave(PULSE).pw(0x400).adsr(0, 9, 9, 6).vib(0.25f, 5.5f, 20).filter(LP, 0x700, 3); add(b); }
	{ B b("Ring Bell", "Sync & Ring"); b.wave(TRI).ring().adsr(0, 10, 0, 10).mod3(17.3f).nofilter(); add(b); }
	{ B b("Ring Gong", "Sync & Ring"); b.wave(TRI).ring().adsr(0, 12, 0, 12).mod3(-5.5f)
		.o2().wave(TRI).semi(12).adsr(0, 11, 0, 11).nofilter(); add(b); }
	{ B b("Metal Clang", "Sync & Ring"); b.wave(TRI).ring().adsr(0, 9, 0, 9).mod3(30.6f).o2().wave(PULSE).pw(0x200).semi(25.3f).adsr(0, 7, 0, 7).nofilter(); add(b); }
	{ B b("Ring Chime", "Sync & Ring"); b.wave(TRI).ring().adsr(0, 10, 3, 9).mod3(24.1f).vib(0.15f, 4.f, 10).nofilter(); add(b); }
	{ B b("Sync Brass", "Sync & Ring"); b.wave(SAW).sync().semi(12).adsr(2, 9, 11, 6).mod3(0).penv(7, 8, 1)
		.filter(LP, 0x500, 3).vib(0.2f, 5.f, 25); add(b); }
	{ B b("Robot Ring", "Sync & Ring"); b.wave(TRI).ring().adsr(0, 9, 12, 6).mod3(0)
		.wt({{K, -36, true}}, -1, 1, 4).nofilter(); add(b); }

	// ---- Keys & Plucks ------------------------------------------------------
	{ B b("Harpsichord", "Keys"); b.wave(PULSE).pw(0x300).adsr(0, 8, 0, 8).o2().wave(PULSE).pw(0x600).semi(12).adsr(0, 7, 0, 7).nofilter(); add(b); }
	{ B b("Electric Piano", "Keys"); b.wave(TRI).adsr(0, 10, 0, 9).o2().wave(PULSE).pw(0x180).semi(12).adsr(0, 5, 0, 5).nofilter(); add(b); }
	{ B b("Clavi", "Keys"); b.wave(PULSE).pw(0x180).adsr(0, 7, 0, 6).filter(LP, 0x500, 11).sweep(-20, 0x280, 0x500); add(b); }
	{ B b("Marimba", "Keys"); b.wave(TRI).adsr(0, 7, 0, 7).wt({{TRI, 24, false}, {K, 0, false}}).nofilter(); add(b); }
	{ B b("Pizzicato", "Keys"); b.wave(SAW).adsr(0, 6, 0, 6).filter(LP, 0x600, 3).sweep(-80, 0x180, 0x600); add(b); }
	{ B b("Music Box", "Keys"); b.wave(TRI).semi(12).adsr(0, 9, 0, 9).o2().wave(TRI).semi(24).adsr(0, 7, 0, 7).nofilter(); add(b); }
	{ B b("Organ", "Keys"); b.wave(PULSE).pw(0x800).adsr(0, 0, 15, 3).o2().wave(TRI).semi(12).adsr(0, 0, 15, 3)
		.o3().wave(TRI).semi(19).adsr(0, 0, 12, 3).filter(LP, 0x700, 0).vib(0.12f, 6.f, 0); add(b); }
	{ B b("Koto", "Keys"); b.wave(PULSE).pw(0x500).pwm(20, 0x300, 0x900).adsr(0, 9, 0, 8).penv(-1.f, 6).filter(LP, 0x500, 6); add(b); }

	// ---- Pads ---------------------------------------------------------------
	{ B b("Warm Pad", "Pad"); b.wave(SAW).adsr(10, 9, 12, 10).o2().wave(SAW).semi(0.15f).adsr(10, 9, 12, 10)
		.filter(LP, 0x300, 6).sweep(2, 0x300, 0x600, true); add(b); }
	{ B b("PWM Strings", "Pad"); b.wave(PULSE).pw(0x400).pwm(12, 0x200, 0xC00).adsr(9, 9, 12, 10)
		.o2().wave(PULSE).pw(0x800).pwm(9, 0x300, 0xD00).semi(0.1f).adsr(9, 9, 12, 10).filter(LP, 0x600, 2); add(b); }
	{ B b("Glass Pad", "Pad"); b.wave(TRI).adsr(11, 9, 13, 11).o2().wave(TRI).semi(12.07f).adsr(11, 9, 11, 11)
		.o3().wave(TRI).semi(19).adsr(11, 9, 9, 11).vib(0.1f, 4.f, 30).nofilter(); add(b); }
	{ B b("Choir", "Pad"); b.wave(PULSE).pw(0x200).adsr(10, 9, 13, 10).o2().wave(PULSE).pw(0x280).semi(0.08f).adsr(10, 9, 13, 10)
		.filter(BP, 0x280, 10).vib(0.15f, 5.f, 20); add(b); }
	{ B b("Dark Drone", "Pad"); b.wave(SAW).adsr(9, 9, 13, 11).o2().wave(SAW).semi(-12.05f).adsr(9, 9, 13, 11)
		.filter(LP, 0x150, 12).sweep(1, 0x150, 0x400, true); add(b); }
	{ B b("Sweep Pad", "Pad"); b.wave(SAW).adsr(11, 9, 13, 11).o2().wave(PULSE).pw(0x600).semi(0.12f).adsr(11, 9, 13, 11)
		.filter(LP, 0x100, 8).sweep(3, 0x100, 0x700); add(b); }
	{ B b("Notch Pad", "Pad"); b.wave(SAW).adsr(9, 9, 12, 10).o2().wave(PULSE).pw(0x400).pwm(10, 0x200, 0xC00).adsr(9, 9, 12, 10)
		.filter(LP | HP, 0x300, 10).sweep(3, 0x200, 0x600, true); add(b); }
	{ B b("Space Strings", "Pad"); b.wave(SAW).adsr(10, 9, 12, 11).o2().wave(SAW).semi(12.1f).adsr(10, 9, 10, 11)
		.o3().wave(TRI).semi(-12).adsr(10, 9, 12, 11).vib(0.1f, 5.f, 30).filter(LP, 0x500, 3); add(b); }

	// ---- Arps & Chords ------------------------------------------------------
	{ B b("Major Arp", "Arp"); b.wave(PULSE).pw(0x400).pwm(20, 0x300, 0xB00).adsr(0, 9, 11, 7).arp({0, 4, 7}).nofilter(); add(b); }
	{ B b("Minor Arp", "Arp"); b.wave(PULSE).pw(0x600).pwm(20, 0x300, 0xB00).adsr(0, 9, 11, 7).arp({0, 3, 7}).nofilter(); add(b); }
	{ B b("Maj7 Arp", "Arp"); b.wave(SAW).adsr(0, 9, 11, 7).arp({0, 4, 7, 11}, 2).filter(LP, 0x500, 6); add(b); }
	{ B b("Octave Arp", "Arp"); b.wave(SAW).adsr(0, 9, 11, 7).arp({0, 12, 24, 12}, 2).filter(LP, 0x600, 4); add(b); }
	{ B b("Sus4 Arp", "Arp"); b.wave(PULSE).pw(0x800).adsr(0, 9, 11, 7).arp({0, 5, 7}).nofilter(); add(b); }
	{ B b("Diminished", "Arp"); b.wave(TRI).adsr(0, 9, 12, 7).arp({0, 3, 6, 9}).nofilter(); add(b); }
	{ B b("Power Chord", "Arp"); b.wave(SAW).adsr(0, 9, 11, 7).arp({0, 7, 12}).filter(LP, 0x380, 9).sweep(-8, 0x200, 0x600); add(b); }
	{ B b("Trill", "Arp"); b.wave(PULSE).pw(0x800).adsr(0, 9, 11, 7).arp({0, 2}, 3).vib(0.1f, 5.f, 0).nofilter(); add(b); }

	// ---- Drums (absolute pitch unless noted) --------------------------------
	{ B b("Kick", "Drums"); b.wave(PULSE).pw(0x800).adsr(0, 8, 0, 8).penv(24, 8)
		.wt({{NOISE, 12, true}, {PULSE, -24, true}, {K, -24, true}, {TRI, -24, true}}).nofilter(); add(b); }
	{ B b("Snare", "Drums"); b.wave(NOISE).adsr(0, 8, 0, 8)
		.wt({{NOISE, 24, true}, {PULSE, -5, true}, {NOISE, 20, true}, {NOISE, 16, true}}).nofilter(); add(b); }
	{ B b("Closed Hat", "Drums"); b.wave(NOISE).adsr(0, 4, 0, 4).wt({{NOISE, 36, true}}).nofilter(); add(b); }
	{ B b("Open Hat", "Drums"); b.wave(NOISE).adsr(0, 8, 0, 8).wt({{NOISE, 36, true}}).nofilter(); add(b); }
	{ B b("Tom (pitched)", "Drums"); b.wave(TRI).adsr(0, 8, 0, 8).penv(12, 10).wt({{NOISE, 12, false}, {TRI, -12, false}}).nofilter(); add(b); }
	{ B b("Clap", "Drums"); b.wave(NOISE).adsr(0, 8, 0, 8)
		.wt({{NOISE, 24, true}, {0, 24, true}, {NOISE, 24, true}, {0, 24, true}, {NOISE, 24, true}}).nofilter(); add(b); }
	{ B b("Laser Tom", "Drums"); b.wave(PULSE).pw(0x800).adsr(0, 8, 0, 8).penv(24, 20).nofilter(); add(b); }
	{ B b("Cowbell", "Drums"); b.wave(PULSE).pw(0x800).semi(19.35f).adsr(0, 7, 0, 7).o2().wave(PULSE).pw(0x800).semi(12.55f).adsr(0, 7, 0, 7)
		.wt({{K, 0, true}}, -1, 1, 3).filter(LP, 0x500, 8); add(b); }

	// ---- FX -----------------------------------------------------------------
	{ B b("Laser", "FX"); b.wave(PULSE).pw(0x800).adsr(0, 9, 0, 9).penv(36, 25).nofilter(); add(b); }
	{ B b("Explosion", "FX"); b.wave(NOISE).adsr(0, 11, 0, 11).wt({{NOISE, -12, true}})
		.filter(LP, 0x700, 4).sweep(-15, 0x080, 0x700); add(b); }
	{ B b("Siren", "FX"); b.wave(PULSE).pw(0x800).adsr(0, 0, 15, 6).vib(5.f, 1.5f, 0).nofilter(); add(b); }
	{ B b("Wind", "FX"); b.wave(NOISE).adsr(12, 9, 15, 12).wt({{NOISE, 12, true}}).filter(LP, 0x200, 15).sweep(4, 0x200, 0x600, true); add(b); }
	{ B b("Bubbles", "FX"); b.wave(TRI).adsr(0, 9, 12, 7).arp({0, 7, 3, 12, 5, 10}, 2).penv(5, 3).nofilter(); add(b); }
	{ B b("Alarm", "FX"); b.wave(PULSE).pw(0x800).adsr(0, 0, 15, 4).arp({0, 12}, 6).nofilter(); add(b); }
	{ B b("Teleport", "FX"); b.wave(SAW).adsr(0, 10, 10, 10).arp({0, 12, 24, 36}).filter(LP, 0x100, 10).sweep(10, 0x100, 0x700, true); add(b); }
	{ B b("Coin", "FX"); b.wave(PULSE).pw(0x800).adsr(0, 8, 0, 8).wt({{K, 0, false}, {K, 5, false}}, -1, 4).nofilter(); add(b); }

	return v;
}

} // namespace

const std::vector<Patch>& factoryPatches() {
	static const std::vector<Patch> bank = build();
	return bank;
}

} // namespace sidv
