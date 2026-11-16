// Offline test harness for the SID6581 voice engine (no VCV Rack needed).
//   render calib            filter cutoff calibration table
//   render all <outdir>     renders every factory patch to WAV and prints levels
//   render bias             filter bias sweep
#include "../src/sidvoice.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sidv;

static const float SR = 48000.f;

static void writeWav(const std::string& path, const std::vector<float>& x) {
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return;
	uint32_t n = x.size(), bytes = n * 2;
	auto u32 = [&](uint32_t v) {std::fwrite(&v, 4, 1, f);};
	auto u16 = [&](uint16_t v) {std::fwrite(&v, 2, 1, f);};
	std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(1); u16(1); u32((uint32_t) SR); u32((uint32_t) SR * 2); u16(2); u16(16);
	std::fwrite("data", 1, 4, f); u32(bytes);
	for (float v : x) {
		int s = (int) std::lround(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f);
		u16((uint16_t) (int16_t) s);
	}
	std::fclose(f);
}

static float rms(const std::vector<float>& x, size_t a, size_t b) {
	double s = 0;
	for (size_t i = a; i < b && i < x.size(); i++)
		s += x[i] * x[i];
	return std::sqrt(s / std::max<size_t>(1, b - a));
}

// Spectral centroid of x[a, a+N) by direct DFT (Hann window)
static float centroid(const std::vector<float>& x, size_t a) {
	const int N = 2048;
	double num = 0, den = 0;
	for (int k = 1; k < N / 2; k += 2) {
		double re = 0, im = 0;
		for (int n = 0; n < N; n++) {
			double w = 0.5 - 0.5 * std::cos(2 * M_PI * n / N);
			double v = x[a + n] * w;
			re += v * std::cos(2 * M_PI * k * n / N);
			im -= v * std::sin(2 * M_PI * k * n / N);
		}
		double m = std::sqrt(re * re + im * im);
		num += m * k * SR / N;
		den += m;
	}
	return den > 0 ? num / den : 0;
}

static std::vector<float> render(const Patch& p, float pitch, float gateSec, float relSec, Mods mods = Mods(), float bias = 0.5f) {
	SidVoice v;
	v.setSampleRate(SR, true);
	v.setFilterBias(bias);
	v.setPatch(&p);
	std::vector<float> out;
	int gateN = gateSec * SR, total = (gateSec + relSec) * SR;
	// Let the chip settle with the gate low
	for (int i = 0; i < 2000; i++)
		v.process(pitch, false, mods);
	for (int i = 0; i < total; i++)
		out.push_back(v.process(pitch, i < gateN, mods));
	return out;
}

int main(int argc, char** argv) {
	std::string mode = argc > 1 ? argv[1] : "all";

	if (mode == "calib" || mode == "bias") {
		Patch p;
		p.osc[0].on = true;
		p.osc[0].wave = SAW;
		p.osc[0].a = 0; p.osc[0].d = 0; p.osc[0].s = 15; p.osc[0].r = 0;
		p.filterMode = LP;
		Patch dry = p;
		dry.osc[0].filter = false;
		auto d = render(dry, -1.f, 0.5f, 0.f);
		std::printf("unfiltered saw C3: rms %.3f centroid %.0f Hz\n", rms(d, 12000, 24000), centroid(d, 12000));
		std::vector<float> biases = mode == "bias" ? std::vector<float>{-1.f, -0.5f, 0.f, 0.5f, 1.f, 1.5f} : std::vector<float>{0.5f};
		for (float b : biases) {
			std::printf("bias %.1f\n", b);
			for (int c = 0; c <= 2047; c += (mode == "bias" ? 256 : 128)) {
				p.cutoff = c;
				auto x = render(p, -1.f, 0.5f, 0.f, Mods(), b);
				std::printf("  cutoff %4d ($%03X): rms %.3f centroid %5.0f Hz\n", c, c, rms(x, 12000, 24000), centroid(x, 12000));
			}
		}
		return 0;
	}

	std::string dir = argc > 2 ? argv[2] : ".";
	const auto& bank = factoryPatches();
	std::printf("%d patches\n", (int) bank.size());
	int idx = 0;
	for (const Patch& p : bank) {
		bool drum = p.category == "Drums";
		float pitch = (p.category == "Bass") ? -2.f : (drum ? 0.f : -1.f + 0.f);
		auto x = render(p, pitch, drum ? 0.3f : 1.2f, drum ? 0.5f : 1.0f);
		float pk = 0;
		for (float v : x)
			pk = std::fmax(pk, std::fabs(v));
		int n = x.size();
		std::printf("%2d %-8s %-16s peak %.2f  rms[0-.2s] %.3f rms[.2-1s] %.3f rms[tail] %.3f  cent %5.0f\n", idx, p.category.c_str(), p.name.c_str(), pk,
			rms(x, 0, SR * 0.2f), rms(x, SR * 0.2f, SR * 1.f), rms(x, n - SR * 0.3f, n), centroid(x, SR * 0.1f));
		char name[256];
		std::snprintf(name, sizeof name, "%s/%02d_%s.wav", dir.c_str(), idx, p.name.c_str());
		for (char* c = name + dir.size() + 1; *c; c++)
			if (*c == ' ' || *c == '/')
				*c = '_';
		writeWav(name, x);
		idx++;
	}
	return 0;
}
