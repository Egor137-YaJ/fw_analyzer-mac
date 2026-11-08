#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <filesystem>
#include <fstream>
#include <span>
#include <algorithm>
#include <cstdlib>
#include <gdr/gdr.hpp>

using namespace geode::prelude;

struct Replay : gdr::Replay<Replay, gdr::Input<>> {
	Replay() : gdr::Replay<Replay, gdr::Input<>>("FWAnalyzer", 1) {}
};

struct Click {
	std::vector<size_t> idx;
	size_t main;
	bool down;
	bool p2;
	int minShift;
	int maxShift;
	int nextFrame;
};

struct State {
	float x1;
	float y1;
	double vel1;
	int flags1;
	float x2;
	float y2;
	double vel2;
	int flags2;
};

const int MAX_SHIFT = 12;

std::vector<gdr::Input<>> g_original;
std::vector<gdr::Input<>> g_inputs;
size_t g_next = 0;
int g_mode = 0;
bool g_trialDone = false;
int g_checkFrom = 0;
int g_limit = -1;

std::vector<Click> g_clicks;
std::vector<Click> g_events;
std::vector<bool> g_noop;
int g_phase = 1;
size_t g_relIndex = 0;
int g_relStep = 0;
bool g_relAllOk = true;
bool g_relTested = false;
const int RELEVANCE_TICKS = 120;
std::vector<int> g_windows;
std::vector<State> g_baseline;
size_t g_clickIndex = 0;
int g_shift = 0;
int g_dir = -1;
int g_lo = 0;
int g_hi = 0;
int g_trials = 0;

std::ofstream g_log;
std::string g_name = "startfa1";
bool g_swap = false;
bool g_human = false;
bool g_verifying = false;
bool g_isWave = true;
const int MIN_HORIZON = 30;
int g_vshift = 0;
Replay g_meta;

std::filesystem::path outDir() {
	return std::filesystem::path(std::getenv("HOME")) / "gd-mods";
}

int playerFlags(PlayerObject* p) {
	int f = 0;
	if (p->m_isUpsideDown) f += 1;
	if (p->m_isShip) f += 2;
	if (p->m_isBird) f += 4;
	if (p->m_isBall) f += 8;
	if (p->m_isDart) f += 16;
	if (p->m_isRobot) f += 32;
	if (p->m_isSpider) f += 64;
	if (p->m_isSwing) f += 128;
	if (p->m_isOnGround) f += 256;
	return f;
}

State readState(GJBaseGameLayer* layer) {
	State s;
	s.x1 = layer->m_player1->m_position.x;
	s.y1 = layer->m_player1->m_position.y;
	s.vel1 = layer->m_player1->m_yVelocity;
	s.flags1 = playerFlags(layer->m_player1);
	s.x2 = layer->m_player2->m_position.x;
	s.y2 = layer->m_player2->m_position.y;
	s.vel2 = layer->m_player2->m_yVelocity;
	s.flags2 = playerFlags(layer->m_player2);
	return s;
}

bool sameState(State a, State b) {
	return a.x1 == b.x1 && a.y1 == b.y1 && a.vel1 == b.vel1 && a.flags1 == b.flags1
		&& a.x2 == b.x2 && a.y2 == b.y2 && a.vel2 == b.vel2 && a.flags2 == b.flags2;
}

bool sortByFrame(gdr::Input<> const& a, gdr::Input<> const& b) {
	return a.frame < b.frame;
}

bool loadReplay() {
	g_name = Mod::get()->getSettingValue<std::string>("replay");
	g_swap = Mod::get()->getSettingValue<bool>("swap-players");
	auto path = Mod::get()->getSaveDir().parent_path() / "eclipse.eclipse-menu" / "replays" / (g_name + ".gdr2");
	auto res = Replay::importData(path);
	if (res.isErr()) {
		FLAlertLayer::create("FW Analyzer", "Can't read replay: " + res.unwrapErr() + "\n" + path.string(), "OK")->show();
		return false;
	}
	g_human = Mod::get()->getSettingValue<bool>("human-macro");
	g_meta = res.unwrap();
	g_original = g_meta.inputs;
	std::stable_sort(g_original.begin(), g_original.end(), sortByFrame);
	return true;
}

void updateBoundsOf(Click& c) {
	int firstFrame = -1;
	int lastFrame = -1;
	for (size_t k = 0; k < c.idx.size(); k++) {
		int f = (int)g_original[c.idx[k]].frame;
		if (firstFrame < 0 || f < firstFrame) {
			firstFrame = f;
		}
		if (f > lastFrame) {
			lastFrame = f;
		}
	}

	int prevFrame = 0;
	int nextFrame = -1;
	for (size_t j = 0; j < g_original.size(); j++) {
		if (std::find(c.idx.begin(), c.idx.end(), j) != c.idx.end()) {
			continue;
		}
		if (g_original[j].player2 != c.p2) {
			continue;
		}
		int f = (int)g_original[j].frame;
		if (f < firstFrame && f > prevFrame) {
			prevFrame = f;
		}
		if (f > lastFrame && (nextFrame < 0 || f < nextFrame)) {
			nextFrame = f;
		}
	}

	c.minShift = prevFrame - firstFrame + 1;
	if (firstFrame + c.minShift < 1) {
		c.minShift = 1 - firstFrame;
	}
	c.maxShift = MAX_SHIFT;
	if (nextFrame >= 0) {
		c.maxShift = nextFrame - lastFrame - 1;
	}
}

void updateBounds(size_t index) {
	updateBoundsOf(g_clicks[index]);
}

std::vector<Click> buildEvents() {
	std::vector<Click> events;
	std::vector<bool> used(g_original.size(), false);
	bool holding[2] = { false, false };

	for (size_t i = 0; i < g_original.size(); i++) {
		if (used[i]) {
			continue;
		}
		bool p2 = g_original[i].player2;
		int player = p2 ? 1 : 0;
		Click c;
		c.p2 = p2;
		c.main = i;
		c.down = false;
		c.minShift = 0;
		c.maxShift = 0;
		c.nextFrame = -1;

		bool pressed = false;
		bool released = false;
		for (size_t j = i; j < g_original.size(); j++) {
			if (g_original[j].frame != g_original[i].frame) {
				break;
			}
			if (used[j] || g_original[j].player2 != p2) {
				continue;
			}
			used[j] = true;
			c.idx.push_back(j);
			if (g_original[j].down && !holding[player]) {
				holding[player] = true;
				if (!pressed) {
					pressed = true;
					c.main = j;
				}
			}
			if (!g_original[j].down && holding[player]) {
				holding[player] = false;
				if (!pressed && !released) {
					released = true;
					c.main = j;
				}
			}
		}

		if (pressed) {
			c.down = true;
			events.push_back(c);
		} else if (released) {
			c.down = false;
			events.push_back(c);
		}
	}

	for (size_t i = 0; i < events.size(); i++) {
		for (size_t j = i + 1; j < events.size(); j++) {
			if (events[j].p2 == events[i].p2) {
				events[i].nextFrame = (int)g_original[events[j].main].frame;
				break;
			}
		}
	}
	return events;
}

void writeHumanMacro() {
	Replay r = g_meta;
	r.inputs = g_original;
	r.sortInputs();
	auto res = r.exportData();
	if (res.isErr()) {
		Notification::create("Can't save human macro", NotificationIcon::Error)->show();
		return;
	}
	auto bytes = res.unwrap();
	std::string fileName = g_name + "_human.gdr2";
	auto replays = Mod::get()->getSaveDir().parent_path() / "eclipse.eclipse-menu" / "replays";

	std::ofstream f1(replays / fileName, std::ios::out | std::ios::binary | std::ios::trunc);
	f1.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	std::ofstream f2(outDir() / fileName, std::ios::out | std::ios::binary | std::ios::trunc);
	f2.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void setSpeed(float speed) {
	CCDirector::get()->getScheduler()->setTimeScale(speed);
}

void requestReset() {
	geode::queueInMainThread([]() {
		if (auto pl = PlayLayer::get()) {
			pl->resetLevel();
		}
	});
}

void prepareTrialFor(Click c) {
	g_inputs = g_original;
	int lastOriginal = 0;
	for (size_t k = 0; k < c.idx.size(); k++) {
		g_inputs[c.idx[k]].frame += g_shift;
		lastOriginal = std::max(lastOriginal, (int)g_original[c.idx[k]].frame);
	}
	std::stable_sort(g_inputs.begin(), g_inputs.end(), sortByFrame);
	g_checkFrom = std::max(lastOriginal, lastOriginal + g_shift) + 2;
	int frame = (int)g_original[c.main].frame;
	g_limit = -1;
	if (g_phase == 0) {
		g_limit = std::max(frame, frame + g_shift) + RELEVANCE_TICKS;
	} else if (c.nextFrame >= 0) {
		g_limit = std::max(frame + g_shift + 2, c.nextFrame - std::abs(g_shift) - 1);
		if (!g_isWave) {
			g_limit = std::max(g_limit, frame + std::abs(g_shift) + MIN_HORIZON);
		}
	}
	g_trials++;
}

int partnerOf(size_t i) {
	for (size_t j = i + 1; j < g_clicks.size(); j++) {
		if (g_clicks[j].p2 == g_clicks[i].p2) {
			return (int)j;
		}
	}
	return -1;
}

int nextPressOf(size_t i) {
	for (size_t j = i + 1; j < g_clicks.size(); j++) {
		if (g_clicks[j].p2 == g_clicks[i].p2 && g_clicks[j].down) {
			return (int)j;
		}
	}
	return -1;
}

bool waveAt(int frame) {
	if (frame < 0 || frame >= (int)g_baseline.size()) {
		return false;
	}
	return (g_baseline[frame].flags1 & 16) != 0;
}

void prepareTrial() {
	Click c = g_clicks[g_clickIndex];
	int frame = (int)g_original[c.main].frame;
	g_isWave = waveAt(frame);
	c.nextFrame = -1;
	int p = partnerOf(g_clickIndex);
	if (!g_isWave) {
		p = nextPressOf(g_clickIndex);
	}
	if (p >= 0) {
		c.nextFrame = (int)g_original[g_clicks[p].main].frame;
	}
	prepareTrialFor(c);
}

void writeFwc(std::string fileName, bool pressesOnly) {
	std::vector<size_t> list;
	for (size_t i = 0; i < g_clicks.size(); i++) {
		if (pressesOnly && !g_clicks[i].down) {
			continue;
		}
		list.push_back(i);
	}

	std::ofstream fwc(outDir() / fileName, std::ios::out | std::ios::binary | std::ios::trunc);
	fwc.write("FWC2", 4);
	double fps = 240.0;
	fwc.write(reinterpret_cast<const char*>(&fps), sizeof(double));
	uint32_t count = (uint32_t)list.size();
	fwc.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
	for (size_t k = 0; k < list.size(); k++) {
		size_t i = list[k];
		auto input = g_original[g_clicks[i].main];
		int32_t frame = (int32_t)input.frame;
		double window = (double)g_windows[i];
		uint8_t flags = 1;
		if (input.player2) {
			flags += 2;
		}
		int32_t ifCount = 1;
		fwc.write(reinterpret_cast<const char*>(&frame), sizeof(int32_t));
		fwc.write(reinterpret_cast<const char*>(&window), sizeof(double));
		fwc.write(reinterpret_cast<const char*>(&flags), sizeof(uint8_t));
		fwc.write(reinterpret_cast<const char*>(&ifCount), sizeof(int32_t));
	}
}

void writeResults() {
	std::ofstream txt(outDir() / (g_name + "_result.txt"), std::ios::out | std::ios::trunc);
	std::map<int, int> pressCounts;
	std::map<int, int> releaseCounts;
	for (size_t i = 0; i < g_clicks.size(); i++) {
		auto input = g_original[g_clicks[i].main];
		std::string type = g_clicks[i].down ? "press" : "release";
		txt << type << " " << i << " frame " << input.frame << " p2 " << input.player2 << " window " << g_windows[i] << "\n";
		if (g_clicks[i].down) {
			pressCounts[g_windows[i]]++;
		} else {
			releaseCounts[g_windows[i]]++;
		}
	}
	txt << "\npresses\n";
	for (auto& [w, n] : pressCounts) {
		txt << "window " << w << ": " << n << "\n";
	}
	int ignored = 0;
	for (size_t i = 0; i < g_noop.size(); i++) {
		if (g_noop[i]) {
			ignored++;
		}
	}
	txt << "\nignored inputs (no effect on the player): " << ignored << "\n";
	txt << "\nreleases\n";
	for (auto& [w, n] : releaseCounts) {
		txt << "window " << w << ": " << n << "\n";
	}

	writeFwc(g_name + "_windows.fwc", false);
	writeFwc(g_name + "_clicks.fwc", true);
}

void finishAnalysis() {
	g_mode = 0;
	setSpeed(1.0f);
	writeResults();
	if (g_human) {
		writeHumanMacro();
	}
	Notification::create("Analysis finished! See gd-mods folder", NotificationIcon::Success)->show();
}

bool shiftAllowed() {
	Click c = g_clicks[g_clickIndex];
	return g_shift >= c.minShift && g_shift <= c.maxShift && g_shift >= -MAX_SHIFT && g_shift <= MAX_SHIFT;
}

void goNextClick();
void clickDone();

void continueSearch() {
	while (g_mode == 2 && !shiftAllowed()) {
		if (g_dir == -1) {
			g_dir = 1;
			g_shift = 1;
		} else {
			clickDone();
			return;
		}
	}
	if (g_mode == 2) {
		prepareTrial();
	}
}

bool verifyAllowed() {
	if (g_vshift <= 0) {
		return true;
	}
	int p = partnerOf(g_clickIndex);
	if (p < 0) {
		return true;
	}
	Click partner = g_clicks[p];
	updateBoundsOf(partner);
	return g_vshift <= partner.maxShift;
}

void prepareVerify() {
	Click c = g_clicks[g_clickIndex];
	int p = partnerOf(g_clickIndex);
	g_inputs = g_original;
	int last = 0;
	for (size_t k = 0; k < c.idx.size(); k++) {
		g_inputs[c.idx[k]].frame += g_vshift;
		last = std::max(last, (int)g_original[c.idx[k]].frame);
	}
	if (p >= 0) {
		Click partner = g_clicks[p];
		for (size_t k = 0; k < partner.idx.size(); k++) {
			g_inputs[partner.idx[k]].frame += g_vshift;
			last = std::max(last, (int)g_original[partner.idx[k]].frame);
		}
	}
	std::stable_sort(g_inputs.begin(), g_inputs.end(), sortByFrame);
	g_checkFrom = std::max(last, last + g_vshift) + 2;
	g_limit = -1;
	g_trials++;
}

void verifyNext() {
	while (g_vshift != 0) {
		if (verifyAllowed()) {
			prepareVerify();
			return;
		}
		if (g_vshift > 0) {
			g_vshift--;
		} else {
			g_vshift++;
		}
	}
	g_verifying = false;
	goNextClick();
}

void verifyAfter(bool ok) {
	if (ok) {
		Click c = g_clicks[g_clickIndex];
		for (size_t k = 0; k < c.idx.size(); k++) {
			g_original[c.idx[k]].frame += g_vshift;
		}
		int p = partnerOf(g_clickIndex);
		if (p >= 0) {
			Click partner = g_clicks[p];
			for (size_t k = 0; k < partner.idx.size(); k++) {
				g_original[partner.idx[k]].frame += g_vshift;
			}
		}
		g_log << "move input " << g_clickIndex << " by " << g_vshift << "\n";
		g_verifying = false;
		goNextClick();
		return;
	}
	if (g_vshift > 0) {
		g_vshift--;
	} else {
		g_vshift++;
	}
	verifyNext();
}

void clickDone() {
	g_windows.push_back(g_hi - g_lo + 1);
	g_log << "click " << g_clickIndex << " frame " << g_original[g_clicks[g_clickIndex].main].frame << " down " << g_clicks[g_clickIndex].down << " lo " << g_lo << " hi " << g_hi << " window " << (g_hi - g_lo + 1) << " wave " << g_isWave << "\n";
	g_log.flush();
	if (g_human) {
		int center = (g_lo + g_hi) / 2;
		if (center != 0) {
			g_verifying = true;
			g_vshift = center;
			verifyNext();
			return;
		}
	}
	goNextClick();
}

void goNextClick() {
	g_clickIndex++;
	if (g_clickIndex % 10 == 0 && g_clickIndex < g_clicks.size()) {
		Notification::create(fmt::format("Input {}/{}", g_clickIndex, g_clicks.size()), NotificationIcon::Info)->show();
	}
	g_dir = -1;
	g_shift = -1;
	g_lo = 0;
	g_hi = 0;
	if (g_clickIndex >= g_clicks.size()) {
		finishAnalysis();
		return;
	}
	updateBounds(g_clickIndex);
	continueSearch();
}

void advanceAfter(bool ok) {
	if (ok) {
		if (g_dir == -1) {
			g_lo = g_shift;
			g_shift--;
		} else {
			g_hi = g_shift;
			g_shift++;
		}
	} else {
		if (g_dir == -1) {
			g_dir = 1;
			g_shift = 1;
		} else {
			clickDone();
			return;
		}
	}
	continueSearch();
}

void startWindowPhase() {
	g_phase = 1;
	g_clicks.clear();
	for (size_t i = 0; i < g_events.size(); i++) {
		if (!g_noop[i]) {
			g_clicks.push_back(g_events[i]);
		}
	}
	for (size_t i = 0; i < g_clicks.size(); i++) {
		g_clicks[i].nextFrame = -1;
		for (size_t j = i + 1; j < g_clicks.size(); j++) {
			if (g_clicks[j].p2 == g_clicks[i].p2) {
				g_clicks[i].nextFrame = (int)g_original[g_clicks[j].main].frame;
				break;
			}
		}
		updateBounds(i);
	}
	g_log << "relevant inputs " << g_clicks.size() << " of " << g_events.size() << "\n";
	g_log.flush();
	Notification::create(fmt::format("{} inputs matter, measuring windows", g_clicks.size()), NotificationIcon::Info)->show();

	g_windows.clear();
	g_clickIndex = 0;
	g_dir = -1;
	g_shift = -1;
	g_lo = 0;
	g_hi = 0;
	if (g_clicks.empty()) {
		finishAnalysis();
		return;
	}
	updateBounds(0);
	continueSearch();
}

void relPrepare() {
	while (g_relIndex < g_events.size()) {
		Click c = g_events[g_relIndex];
		while (g_relStep < 2) {
			int s = -1;
			if (g_relStep == 1) {
				s = 1;
			}
			if (s >= c.minShift && s <= c.maxShift) {
				g_shift = s;
				prepareTrialFor(c);
				return;
			}
			g_relStep++;
		}
		g_noop[g_relIndex] = g_relTested && g_relAllOk;
		g_log << "input " << g_relIndex << " frame " << g_original[c.main].frame << " down " << c.down << " noop " << g_noop[g_relIndex] << "\n";
		g_relIndex++;
		g_relStep = 0;
		g_relAllOk = true;
		g_relTested = false;
	}
	startWindowPhase();
}

void relAfter(bool ok) {
	g_relTested = true;
	if (!ok) {
		g_relAllOk = false;
		g_relStep = 2;
	} else {
		g_relStep++;
	}
	relPrepare();
}

void finishTrial(bool ok) {
	if (g_trialDone) {
		return;
	}
	g_trialDone = true;
	if (g_phase == 0) {
		relAfter(ok);
	} else if (g_verifying) {
		verifyAfter(ok);
	} else {
		advanceAfter(ok);
	}
	if (g_mode == 2) {
		requestReset();
	}
}

void startTrials() {
	g_windows.clear();
	g_trials = 0;
	g_mode = 2;
	g_noop.clear();
	g_verifying = false;
	g_phase = 0;
	g_events = buildEvents();
	for (size_t i = 0; i < g_events.size(); i++) {
		updateBoundsOf(g_events[i]);
	}
	g_noop.assign(g_events.size(), false);
	g_relIndex = 0;
	g_relStep = 0;
	g_relAllOk = true;
	g_relTested = false;
	g_log << "baseline ticks " << g_baseline.size() << " inputs " << g_events.size() << "\n";
	g_log.flush();
	relPrepare();
	if (g_mode == 2) {
		requestReset();
	}
}

void levelEnded() {
	if (g_trialDone) {
		return;
	}
	if (g_mode == 1) {
		g_trialDone = true;
		Notification::create("Baseline OK, starting analysis", NotificationIcon::Success)->show();
		setSpeed((float)Mod::get()->getSettingValue<double>("speed"));
		startTrials();
		return;
	}
	g_log << "trial click " << g_clickIndex << " shift " << g_shift << " ok (completed)\n";
	finishTrial(true);
}

class $modify(MyMenuLayer, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) {
			return false;
		}

		auto menu = this->getChildByID("bottom-menu");
		auto btn = CCMenuItemSpriteExtra::create(
			CircleButtonSprite::createWithSpriteFrameName("GJ_timeIcon_001.png"),
			this,
			menu_selector(MyMenuLayer::onAnalyze)
		);
		menu->addChild(btn);
		menu->updateLayout();

		return true;
	}

	void onAnalyze(CCObject*) {
		if (!loadReplay()) {
			return;
		}
		g_log.close();
		g_log.open(outDir() / "fwa_log.txt", std::ios::out | std::ios::trunc);
		g_inputs = g_original;
		g_baseline.clear();
		g_mode = 1;
		g_next = 0;
		std::string text = fmt::format("Loaded {} inputs.\nOpen the level in normal mode\nand don't touch anything.", g_original.size());
		FLAlertLayer::create("FW Analyzer", text, "OK")->show();
	}
};

class $modify(MyBaseLayer, GJBaseGameLayer) {
	void processQueuedButtons(float dt, bool clearInputQueue) {
		if (g_mode != 0 && PlayLayer::get() && !g_trialDone) {
			int tick = m_gameState.m_currentProgress / 2;
			while (g_next < g_inputs.size() && (int)g_inputs[g_next].frame <= tick) {
				auto input = g_inputs[g_next];
				bool isPlayer1 = !input.player2;
				if (g_swap) {
					isPlayer1 = input.player2;
				}
				this->handleButton(input.down, input.button, isPlayer1);
				g_next++;
			}

			State now = readState(this);
			if (g_mode == 1) {
				while ((int)g_baseline.size() <= tick) {
					g_baseline.push_back(now);
				}
			}
			auto pl = PlayLayer::get();
			if (g_mode == 1 && tick % 240 == 0) {
				g_log << "base tick " << tick << " percent " << pl->getCurrentPercent() << " x " << now.x1 << " end " << pl->m_levelEndAnimationStarted << "\n";
				g_log.flush();
			}
			if (pl->m_levelEndAnimationStarted || pl->getCurrentPercent() >= 100.0f) {
				g_log << "end detected tick " << tick << "\n";
				levelEnded();
			}
			if (!g_trialDone && g_mode == 2 && g_limit >= 0 && tick >= g_limit) {
				if (g_phase == 0) {
					finishTrial(false);
				} else {
					g_log << "trial click " << g_clickIndex << " shift " << g_shift << " ok (survived to " << tick << ")\n";
					finishTrial(true);
				}
			}
			if (!g_trialDone && g_mode == 2 && tick >= g_checkFrom && tick < (int)g_baseline.size()) {
				if (sameState(now, g_baseline[tick])) {
					g_log << "trial click " << g_clickIndex << " shift " << g_shift << " ok (same state at " << tick << ")\n";
					finishTrial(true);
				} else if (g_phase == 0) {
					finishTrial(false);
				}
			}
		}
		GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
	}
};

class $modify(MyPlayLayer, PlayLayer) {
	void resetLevel() {
		g_next = 0;
		g_trialDone = false;
		PlayLayer::resetLevel();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (object == m_anticheatSpike || g_mode == 0) {
			PlayLayer::destroyPlayer(player, object);
			return;
		}
		if (g_trialDone) {
			return;
		}
		if (g_mode == 1) {
			g_mode = 0;
			setSpeed(1.0f);
			Notification::create(fmt::format("Baseline died at tick {}", m_gameState.m_currentProgress / 2), NotificationIcon::Error)->show();
			PlayLayer::destroyPlayer(player, object);
			return;
		}
		g_log << "trial click " << g_clickIndex << " shift " << g_shift << " died at " << m_gameState.m_currentProgress / 2 << "\n";
		finishTrial(false);
	}

	void playEndAnimationToPos(CCPoint position) {
		g_log << "playEndAnimationToPos\n";
		g_log.flush();
		PlayLayer::playEndAnimationToPos(position);
		if (g_mode != 0) {
			levelEnded();
		}
	}

	void showCompleteText() {
		g_log << "showCompleteText\n";
		g_log.flush();
		PlayLayer::showCompleteText();
	}

	void levelComplete() {
		g_log << "levelComplete\n";
		g_log.flush();
		if (g_mode == 0) {
			PlayLayer::levelComplete();
			return;
		}
		levelEnded();
	}

	void onQuit() {
		g_mode = 0;
		setSpeed(1.0f);
		PlayLayer::onQuit();
	}
};
