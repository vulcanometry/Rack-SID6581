#include "plugin.hpp"
#include "sidvoice.hpp"

// SID6581: a four-voice VCO built on an emulated MOS 6581 (reSID). Each voice
// is a whole SID chip with its own filter, playing one of 64 factory patches.

namespace {

const int VOICES = 4;
// Default patch per voice: Pulse Bass, Square Lead, Warm Pad, Major Arp
const int DEFAULT_PATCH[VOICES] = {0, 8, 32, 40};

int bankSize() {
	return (int) sidv::factoryPatches().size();
}

} // namespace


struct SID6581 : Module {
	enum ParamId {
		ENUMS(PATCH_PARAM, VOICES),
		ENUMS(OCT_PARAM, VOICES),
		ENUMS(TUNE_PARAM, VOICES),
		ENUMS(PW_PARAM, VOICES),
		ENUMS(CUTOFF_PARAM, VOICES),
		ENUMS(RES_PARAM, VOICES),
		ENUMS(LEVEL_PARAM, VOICES),
		BIAS_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(VOCT_INPUT, VOICES),
		ENUMS(GATE_INPUT, VOICES),
		ENUMS(PW_INPUT, VOICES),
		ENUMS(CUTOFF_INPUT, VOICES),
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(VOICE_OUTPUT, VOICES),
		MIX_OUTPUT,
		POLY_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(LEVEL_LIGHT, VOICES),
		ENUMS(DRONE_LIGHT, VOICES),
		LIGHTS_LEN
	};

	sidv::SidVoice voices[VOICES];
	int currentPatch[VOICES];
	bool gateIn[VOICES] = {};
	float level[VOICES] = {};
	bool drone[VOICES] = {};

	// Settings from the context menu, applied on the audio thread
	bool mos8580 = false;
	bool highQuality = true;
	std::atomic<bool> configDirty {true};
	float appliedBias = -100.f;
	dsp::ClockDivider lightDivider;

	SID6581() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		std::vector<std::string> names;
		for (const sidv::Patch& p : sidv::factoryPatches())
			names.push_back(p.name);
		const int n = std::max(1, bankSize());

		for (int v = 0; v < VOICES; v++) {
			std::string vn = string::f("Voice %d ", v + 1);
			configSwitch(PATCH_PARAM + v, 0.f, n - 1, std::min(DEFAULT_PATCH[v], n - 1), vn + "patch", names);
			configParam(OCT_PARAM + v, -4.f, 3.f, 0.f, vn + "octave");
			paramQuantities[OCT_PARAM + v]->snapEnabled = true;
			configParam(TUNE_PARAM + v, -12.f, 12.f, 0.f, vn + "tune", " semitones");
			paramQuantities[TUNE_PARAM + v]->randomizeEnabled = false;
			configParam(PW_PARAM + v, -1.f, 1.f, 0.f, vn + "pulse width offset", "%", 0.f, 100.f);
			configParam(CUTOFF_PARAM + v, -1.f, 1.f, 0.f, vn + "cutoff offset", "%", 0.f, 100.f);
			configParam(RES_PARAM + v, -1.f, 1.f, 0.f, vn + "resonance offset", "%", 0.f, 100.f);
			configParam(LEVEL_PARAM + v, 0.f, 1.f, 1.f, vn + "level", "%", 0.f, 100.f);
			paramQuantities[LEVEL_PARAM + v]->randomizeEnabled = false;

			configInput(VOCT_INPUT + v, vn + "1V/octave pitch");
			configInput(GATE_INPUT + v, vn + "gate");
			configInput(PW_INPUT + v, vn + "pulse width CV");
			configInput(CUTOFF_INPUT + v, vn + "cutoff CV");
			configOutput(VOICE_OUTPUT + v, vn + "audio");
			configLight(LEVEL_LIGHT + v, vn + "level");
			configLight(DRONE_LIGHT + v, vn + "drone (no gate patched)");
			currentPatch[v] = -1;
		}
		configParam(BIAS_PARAM, -1.f, 1.5f, 0.5f, "6581 filter bias (dark / bright chip)", " V");
		paramQuantities[BIAS_PARAM]->randomizeEnabled = false;
		configOutput(MIX_OUTPUT, "Mix of all voices");
		configOutput(POLY_OUTPUT, "Voices 1-4 (polyphonic)");

		lightDivider.setDivision(64);
	}

	int patchIndex(int v) {
		return clamp((int) std::round(params[PATCH_PARAM + v].getValue()), 0, bankSize() - 1);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		configDirty = true;
	}

	void applyConfig(float sampleRate) {
		for (sidv::SidVoice& v : voices) {
			v.setChipModel(mos8580);
			v.setSampleRate(sampleRate, highQuality);
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "mos8580", json_boolean(mos8580));
		json_object_set_new(rootJ, "highQuality", json_boolean(highQuality));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		mos8580 = json_is_true(json_object_get(rootJ, "mos8580"));
		json_t* hqJ = json_object_get(rootJ, "highQuality");
		highQuality = hqJ ? json_is_true(hqJ) : true;
		configDirty = true;
	}

	// An unpatched V/OCT or GATE jack is normalled to the nearest patched jack
	// of a lower voice: it takes the next channel of a polyphonic cable, or
	// shares a mono one (so one mono cable layers all four voices).
	bool source(int inputBase, int v, int* jack, int* channel) {
		for (int j = v; j >= 0; j--) {
			Input& in = inputs[inputBase + j];
			if (!in.isConnected())
				continue;
			int c = v - j;
			*jack = inputBase + j;
			*channel = c < in.getChannels() ? c : 0;
			return true;
		}
		return false;
	}

	void process(const ProcessArgs& args) override {
		if (configDirty.exchange(false))
			applyConfig(args.sampleRate);

		const float bias = params[BIAS_PARAM].getValue();
		if (bias != appliedBias) {
			appliedBias = bias;
			for (sidv::SidVoice& v : voices)
				v.setFilterBias(bias);
		}

		const auto& bank = sidv::factoryPatches();
		if (bank.empty())
			return;

		const bool anyShared = outputs[MIX_OUTPUT].isConnected() || outputs[POLY_OUTPUT].isConnected();
		float mix = 0.f;

		for (int v = 0; v < VOICES; v++) {
			int jack, ch;
			const bool hasGate = source(GATE_INPUT, v, &jack, &ch);
			bool gate = true;
			if (hasGate) {
				float g = inputs[jack].getVoltage(ch);
				if (gateIn[v])
					gateIn[v] = g > 0.5f;
				else
					gateIn[v] = g >= 1.f;
				gate = gateIn[v];
			}
			drone[v] = !hasGate;

			// A new patch restarts a held note so it is heard straight away
			int idx = patchIndex(v);
			if (idx != currentPatch[v]) {
				currentPatch[v] = idx;
				voices[v].setPatch(&bank[idx]);
				voices[v].retrigger();
			}

			if (!outputs[VOICE_OUTPUT + v].isConnected() && !anyShared) {
				level[v] = 0.f;
				continue;
			}

			float pitch = source(VOCT_INPUT, v, &jack, &ch) ? inputs[jack].getVoltage(ch) : 0.f;
			pitch += params[OCT_PARAM + v].getValue() + params[TUNE_PARAM + v].getValue() / 12.f;

			sidv::Mods m;
			m.pw = (params[PW_PARAM + v].getValue() + inputs[PW_INPUT + v].getVoltage() * 0.2f) * 2048.f;
			m.cutoff = (params[CUTOFF_PARAM + v].getValue() + inputs[CUTOFF_INPUT + v].getVoltage() * 0.2f) * 1024.f;
			m.res = params[RES_PARAM + v].getValue() * 15.f;
			m.drone = drone[v];

			float out = voices[v].process(pitch, gate, m) * 10.f * params[LEVEL_PARAM + v].getValue();
			outputs[VOICE_OUTPUT + v].setVoltage(out);
			outputs[POLY_OUTPUT].setVoltage(out, v);
			mix += out;
			level[v] = std::max(level[v] * 0.9995f, std::fabs(out));
		}

		// Soft limit the sum to about +/-10V
		outputs[MIX_OUTPUT].setVoltage(10.f * std::tanh(mix * 0.05f));
		outputs[POLY_OUTPUT].setChannels(VOICES);

		if (lightDivider.process()) {
			float dt = args.sampleTime * lightDivider.getDivision();
			for (int v = 0; v < VOICES; v++) {
				lights[LEVEL_LIGHT + v].setBrightnessSmooth(std::min(1.f, level[v] / 5.f), dt);
				lights[DRONE_LIGHT + v].setBrightness(drone[v] ? 0.6f : 0.f);
			}
		}
	}
};


// ---------------------------------------------------------------------------
// Panel

namespace {

NVGcolor c64Blue() { return nvgRGB(0x35, 0x28, 0x79); }
NVGcolor c64LightBlue() { return nvgRGB(0x86, 0x7a, 0xde); }
NVGcolor ink() { return nvgRGB(0x3a, 0x30, 0x26); }
NVGcolor cream() { return nvgRGB(0xee, 0xe4, 0xcc); }

// Categories in bank order
std::vector<std::string> categories() {
	std::vector<std::string> cats;
	for (const sidv::Patch& p : sidv::factoryPatches()) {
		if (std::find(cats.begin(), cats.end(), p.category) == cats.end())
			cats.push_back(p.category);
	}
	return cats;
}

// Sets a voice's patch as one undoable action
void selectPatch(SID6581* module, int voice, int index) {
	int id = SID6581::PATCH_PARAM + voice;
	ParamQuantity* pq = module->paramQuantities[id];
	float oldValue = pq->getValue();
	float newValue = clamp(index, 0, bankSize() - 1);
	if (oldValue == newValue)
		return;
	pq->setValue(newValue);
	auto* h = new history::ParamChange;
	h->name = "change SID6581 patch";
	h->moduleId = module->id;
	h->paramId = id;
	h->oldValue = oldValue;
	h->newValue = newValue;
	APP->history->push(h);
}

void appendPatchMenu(ui::Menu* menu, SID6581* module, int voice) {
	const auto& bank = sidv::factoryPatches();
	for (const std::string& cat : categories()) {
		int current = module->patchIndex(voice);
		bool inCat = bank[current].category == cat;
		menu->addChild(createSubmenuItem(cat, inCat ? bank[current].name : "", [=](ui::Menu* sub) {
			const auto& bank = sidv::factoryPatches();
			for (int i = 0; i < (int) bank.size(); i++) {
				if (bank[i].category != cat)
					continue;
				sub->addChild(createCheckMenuItem(string::f("%02d  %s", i + 1, bank[i].name.c_str()), "",
					[=]() {return module->patchIndex(voice) == i;},
					[=]() {selectPatch(module, voice, i);}
				));
			}
		}));
	}
}

} // namespace


// Static artwork: background, header, section boxes and labels.
struct PanelDecor : TransparentWidget {
	struct Label {
		Vec pos;
		std::string text;
		float size;
		NVGcolor color;
	};
	struct Box {
		math::Rect rect;
		std::string title;
		NVGcolor fill;
	};
	std::vector<Box> boxes;
	std::vector<Label> labels;
	math::Rect chipRect;

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		std::shared_ptr<window::Font> mono = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));

		// Breadbin beige case with a subtle vertical gradient
		nvgBeginPath(vg);
		nvgRect(vg, 0, 0, box.size.x, box.size.y);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, box.size.y, nvgRGB(0xd9, 0xcf, 0xb6), nvgRGB(0xc4, 0xb8, 0x9b)));
		nvgFill(vg);

		// Rainbow badge stripes in the header
		const NVGcolor stripes[5] = {
			nvgRGB(0xc8, 0x3a, 0x2c), nvgRGB(0xe0, 0x7a, 0x22), nvgRGB(0xe8, 0xc0, 0x2a),
			nvgRGB(0x4c, 0x9a, 0x3c), nvgRGB(0x2e, 0x5c, 0xa8),
		};
		for (int i = 0; i < 5; i++) {
			nvgBeginPath(vg);
			nvgRect(vg, box.size.x - mm2px(50.f), mm2px(5.f + i * 1.25f), mm2px(34.f), mm2px(0.85f));
			nvgFillColor(vg, stripes[i]);
			nvgFill(vg);
		}

		for (const Box& b : boxes) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, b.rect.pos.x, b.rect.pos.y, b.rect.size.x, b.rect.size.y, 3.f);
			nvgFillColor(vg, b.fill);
			nvgFill(vg);
			nvgStrokeColor(vg, nvgRGB(0x24, 0x1c, 0x14));
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
			if (font && !b.title.empty()) {
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, 7.5f);
				nvgTextLetterSpacing(vg, 1.f);
				nvgFillColor(vg, nvgRGB(0xe8, 0xc0, 0x2a));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
				nvgText(vg, b.rect.pos.x + b.rect.size.x / 2.f, b.rect.pos.y + mm2px(1.3f), b.title.c_str(), NULL);
			}
		}

		drawChip(vg, mono);

		if (!font)
			return;
		nvgFontFaceId(vg, font->handle);
		nvgFontSize(vg, 22.f);
		nvgTextLetterSpacing(vg, 2.f);
		nvgFillColor(vg, ink());
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgText(vg, mm2px(13.f), mm2px(10.6f), "SID 6581", NULL);
		nvgFontSize(vg, 7.f);
		nvgTextLetterSpacing(vg, 1.f);
		nvgText(vg, mm2px(56.f), mm2px(10.6f), "4-VOICE SOUND INTERFACE VCO", NULL);

		nvgTextLetterSpacing(vg, 0.4f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		for (const Label& l : labels) {
			nvgFontSize(vg, l.size);
			nvgFillColor(vg, l.color);
			nvgText(vg, l.pos.x, l.pos.y, l.text.c_str(), NULL);
		}
	}

	// A 28-pin DIP package with its markings
	void drawChip(NVGcontext* vg, std::shared_ptr<window::Font> mono) {
		const math::Rect& r = chipRect;
		if (r.size.x <= 0)
			return;
		const int pins = 14;
		const float pinW = mm2px(1.6f), pinH = mm2px(0.9f);
		for (int i = 0; i < pins; i++) {
			float y = r.pos.y + r.size.y * (i + 0.5f) / pins - pinH / 2.f;
			for (float x : {r.pos.x - pinW + 1.f, r.pos.x + r.size.x - 1.f}) {
				nvgBeginPath(vg);
				nvgRect(vg, x, y, pinW, pinH);
				nvgFillColor(vg, nvgRGB(0xb8, 0xb8, 0xb0));
				nvgFill(vg);
			}
		}
		nvgBeginPath(vg);
		nvgRoundedRect(vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 2.f);
		nvgFillColor(vg, nvgRGB(0x1c, 0x1a, 0x18));
		nvgFill(vg);
		// Notch
		nvgBeginPath(vg);
		nvgArc(vg, r.pos.x + r.size.x / 2.f, r.pos.y, mm2px(1.5f), 0, M_PI, NVG_CW);
		nvgFillColor(vg, nvgRGB(0x0c, 0x0a, 0x08));
		nvgFill(vg);
		if (!mono)
			return;
		nvgFontFaceId(vg, mono->handle);
		nvgFillColor(vg, nvgRGB(0xc8, 0xc4, 0xb8));
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgSave(vg);
		nvgTranslate(vg, r.pos.x + r.size.x / 2.f, r.pos.y + r.size.y / 2.f);
		nvgRotate(vg, -M_PI / 2.f);
		nvgFontSize(vg, 11.f);
		nvgTextLetterSpacing(vg, 1.f);
		nvgText(vg, 0, -mm2px(1.4f), "MOS 6581", NULL);
		nvgFontSize(vg, 7.f);
		nvgText(vg, 0, mm2px(1.9f), "SOUND INTERFACE", NULL);
		nvgRestore(vg);
	}
};


// The patch display: C64 screen colours, patch number and category above the name.
// Left-click opens the patch browser.
struct PatchDisplay : widget::Widget {
	SID6581* module = NULL;
	int voice = 0;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, c64Blue());
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, c64LightBlue());
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		const auto& bank = sidv::factoryPatches();
		if (!font || bank.empty())
			return;
		int idx = module ? module->patchIndex(voice) : DEFAULT_PATCH[voice];
		idx = clamp(idx, 0, (int) bank.size() - 1);
		std::string head = string::f("%02d %s", idx + 1, string::uppercase(bank[idx].category).c_str());
		std::string name = string::uppercase(bank[idx].name);

		NVGcontext* vg = args.vg;
		nvgSave(vg);
		nvgScissor(vg, 1.f, 0, box.size.x - 2.f, box.size.y);
		nvgFontFaceId(vg, font->handle);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, c64LightBlue());
		nvgTextLetterSpacing(vg, 0.3f);
		nvgFontSize(vg, 8.f);
		nvgText(vg, box.size.x / 2.f, box.size.y * 0.3f, head.c_str(), NULL);

		// Shrink long names to fit
		float size = 11.f, bounds[4];
		for (; size > 6.f; size -= 0.5f) {
			nvgFontSize(vg, size);
			nvgTextBounds(vg, 0, 0, name.c_str(), NULL, bounds);
			if (bounds[2] - bounds[0] <= box.size.x - 4.f)
				break;
		}
		nvgFontSize(vg, size);
		nvgFillColor(vg, nvgRGB(0xd8, 0xd2, 0xff));
		nvgText(vg, box.size.x / 2.f, box.size.y * 0.7f, name.c_str(), NULL);
		nvgRestore(vg);
	}

	void onButton(const ButtonEvent& e) override {
		// Right-click falls through to the module's context menu
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("Voice %d patch", voice + 1)));
			appendPatchMenu(menu, module, voice);
			e.consume(this);
		}
	}
};


// Previous / next patch arrow
struct PatchArrow : widget::Widget {
	SID6581* module = NULL;
	int voice = 0;
	int delta = 1;
	bool hovered = false;

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float cx = box.size.x / 2.f, cy = box.size.y / 2.f, s = std::min(box.size.x, box.size.y) * 0.28f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, cx + delta * s, cy);
		nvgLineTo(vg, cx - delta * s, cy - s * 1.2f);
		nvgLineTo(vg, cx - delta * s, cy + s * 1.2f);
		nvgClosePath(vg);
		nvgFillColor(vg, hovered ? nvgRGB(0xff, 0xd8, 0x50) : nvgRGB(0xe8, 0xc0, 0x2a));
		nvgFill(vg);
	}

	void onEnter(const EnterEvent& e) override {
		hovered = true;
	}
	void onLeave(const LeaveEvent& e) override {
		hovered = false;
	}
	void onHover(const HoverEvent& e) override {
		e.consume(this);
	}

	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			int n = bankSize();
			if (n > 0)
				selectPatch(module, voice, ((module->patchIndex(voice) + delta) % n + n) % n);
			e.consume(this);
		}
	}
};


struct SID6581Widget : ModuleWidget {
	PanelDecor* decor;

	SID6581Widget(SID6581* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SID6581.svg")));

		auto* fb = new widget::FramebufferWidget;
		fb->box.size = box.size;
		decor = new PanelDecor;
		decor->box.size = box.size;
		fb->addChild(decor);
		addChild(fb);

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using M = SID6581;
		const float top = 14.f, bottom = 120.5f;
		const float colW = 28.f, gap = 2.f, x0 = 4.f;
		const NVGcolor sectionFill = nvgRGB(0x4a, 0x3f, 0x35);

		for (int v = 0; v < VOICES; v++) {
			const float x = x0 + v * (colW + gap);
			const float cx = x + colW / 2.f;
			const float l = cx - 6.5f, r = cx + 6.5f;
			decor->boxes.push_back({math::Rect(mm2px(Vec(x, top)), mm2px(Vec(colW, bottom - top))), string::f("VOICE %d", v + 1), sectionFill});

			// Patch display between its arrows
			const float dy = top + 5.5f, dh = 10.f, aw = 4.f;
			auto* prev = new PatchArrow;
			prev->module = module;
			prev->voice = v;
			prev->delta = -1;
			prev->box = math::Rect(mm2px(Vec(x + 0.5f, dy)), mm2px(Vec(aw, dh)));
			addChild(prev);
			auto* display = new PatchDisplay;
			display->module = module;
			display->voice = v;
			display->box = math::Rect(mm2px(Vec(x + 0.5f + aw, dy)), mm2px(Vec(colW - 1.f - 2 * aw, dh)));
			addChild(display);
			auto* next = new PatchArrow;
			next->module = module;
			next->voice = v;
			next->delta = 1;
			next->box = math::Rect(mm2px(Vec(x + colW - 0.5f - aw, dy)), mm2px(Vec(aw, dh)));
			addChild(next);

			knob<RoundBlackSnapKnob>(l, 36.f, M::OCT_PARAM + v, "OCTAVE");
			knob<RoundBlackKnob>(r, 36.f, M::TUNE_PARAM + v, "TUNE");
			knob<RoundBlackKnob>(l, 50.f, M::PW_PARAM + v, "PW");
			knob<RoundBlackKnob>(r, 50.f, M::CUTOFF_PARAM + v, "CUTOFF");
			knob<RoundBlackKnob>(l, 64.f, M::RES_PARAM + v, "RES");
			knob<RoundBlackKnob>(r, 64.f, M::LEVEL_PARAM + v, "LEVEL");

			jack(l, 84.f, M::VOCT_INPUT + v, true, "V/OCT");
			jack(r, 84.f, M::GATE_INPUT + v, true, "GATE");
			jack(l, 97.f, M::PW_INPUT + v, true, "PW");
			jack(r, 97.f, M::CUTOFF_INPUT + v, true, "CUTOFF");

			// Output on a dark plate with the level and drone lights
			decor->boxes.push_back({math::Rect(mm2px(Vec(x + 3.f, 104.f)), mm2px(Vec(colW - 6.f, 14.5f))), "", nvgRGB(0x24, 0x1e, 0x18)});
			jack(cx, 112.f, M::VOICE_OUTPUT + v, false, "OUT");
			addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(cx - 8.f, 112.f)), module, M::LEVEL_LIGHT + v));
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(cx + 8.f, 112.f)), module, M::DRONE_LIGHT + v));
			label(Vec(cx - 8.f, 115.6f), "LVL", 5.5f);
			label(Vec(cx + 8.f, 115.6f), "DRONE", 5.5f);
		}

		// Chip column
		const float gx = x0 + VOICES * (colW + gap);
		const float gw = box.size.x / mm2px(1.f) - 4.f - gx;
		const float gcx = gx + gw / 2.f;
		decor->boxes.push_back({math::Rect(mm2px(Vec(gx, top)), mm2px(Vec(gw, bottom - top))), "CHIP", sectionFill});
		knob<RoundBlackKnob>(gcx, 26.f, M::BIAS_PARAM, "BIAS");
		label(Vec(gcx, 36.6f), "DARK - BRIGHT", 5.5f);
		decor->chipRect = math::Rect(mm2px(Vec(gcx - 4.5f, 41.f)), mm2px(Vec(9.f, 38.f)));

		decor->boxes.push_back({math::Rect(mm2px(Vec(gx + 2.f, 86.5f)), mm2px(Vec(gw - 4.f, 32.f))), "", nvgRGB(0x24, 0x1e, 0x18)});
		jack(gcx, 97.f, M::POLY_OUTPUT, false, "POLY");
		jack(gcx, 112.f, M::MIX_OUTPUT, false, "MIX");
	}

	void appendContextMenu(ui::Menu* menu) override {
		SID6581* module = getModule<SID6581>();
		if (!module)
			return;
		menu->addChild(new ui::MenuSeparator);
		const auto& bank = sidv::factoryPatches();
		for (int v = 0; v < VOICES; v++) {
			menu->addChild(createSubmenuItem(string::f("Voice %d patch", v + 1), bank.empty() ? "" : bank[module->patchIndex(v)].name, [=](ui::Menu* sub) {
				appendPatchMenu(sub, module, v);
			}));
		}
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Chip model", {"MOS 6581", "MOS 8580"},
			[=]() {return module->mos8580 ? 1 : 0;},
			[=](size_t i) {module->mos8580 = i == 1; module->configDirty = true;}
		));
		menu->addChild(createIndexSubmenuItem("Resampling", {"High quality (FIR)", "Low CPU (interpolate)"},
			[=]() {return module->highQuality ? 0 : 1;},
			[=](size_t i) {module->highQuality = i == 0; module->configDirty = true;}
		));
	}

	void label(Vec mm, const char* text, float size) {
		decor->labels.push_back({mm2px(mm), text, size, cream()});
	}

	template <class TKnob>
	void knob(float x, float y, int paramId, const char* text) {
		auto* w = createParamCentered<TKnob>(mm2px(Vec(x, y)), module, paramId);
		addParam(w);
		label(Vec(x, y + 7.2f), text, 6.5f);
	}

	void jack(float x, float y, int id, bool input, const char* text) {
		if (input)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, id));
		else
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, id));
		label(Vec(x, y - 5.6f), text, 6.f);
	}
};


Model* modelSID6581 = createModel<SID6581, SID6581Widget>("SID6581");
