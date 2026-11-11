#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <filesystem>
#include <fstream>
#include <span>
#include <algorithm>
#include <cstdlib>
#include <cmath>
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
bool g_waitStart = true;
bool g_block = false;
cocos2d::CCArray* g_cpArray = nullptr;
std::vector<int> g_cpTick;
std::vector<bool> g_cpOk;
int g_loadCp = -1;
size_t g_cpTest = 0;
int g_cpFirstTick = -1;
int g_tickOffset = 0;
bool g_findOffset = false;
bool g_held[2] = { false, false };
int g_nextCpTick = 120;
int g_lastInputTick = -100;
const int CP_EVERY = 240;
std::vector<State> g_tmpBase;
int g_tmpStart = -1;
std::map<int, bool> g_tmpMatch;
const int CP_TEST_TICKS = 60;
int g_pass = 1;
int g_moves = 0;
bool g_collapseDone = false;
bool g_resetQueued = false;
int g_collapseCount = 0;
int g_collapseFrom = -1;
size_t g_cutA = 0;
size_t g_cutC = 0;
size_t g_cutD = 0;
bool g_hasD = false;
int g_variant = 1;
int g_cutLen = 0;
const int MICRO_TICKS = 16;
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
	if (p->m_vehicleSize < 1.0f) f += 512;
	return f;
}

State readState(GJBaseGameLayer* layer) {
	State s;
	s.x1 = layer->m_player1->getPositionX();
	s.y1 = layer->m_player1->getPositionY();
	s.vel1 = layer->m_player1->m_yVelocity;
	s.flags1 = playerFlags(layer->m_player1);
	s.x2 = layer->m_player2->getPositionX();
	s.y2 = layer->m_player2->getPositionY();
	s.vel2 = layer->m_player2->m_yVelocity;
	s.flags2 = playerFlags(layer->m_player2);
	return s;
}

bool closeEnough(double a, double b) {
	return std::abs(a - b) < 0.01;
}

bool sameState(State a, State b) {
	return closeEnough(a.x1, b.x1) && closeEnough(a.y1, b.y1) && closeEnough(a.vel1, b.vel1) && a.flags1 == b.flags1
		&& closeEnough(a.x2, b.x2) && closeEnough(a.y2, b.y2) && closeEnough(a.vel2, b.vel2) && a.flags2 == b.flags2;
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

bool waveAt(int frame);

void updateBounds(size_t index) {
	updateBoundsOf(g_clicks[index]);
	g_block = g_human && waveAt((int)g_original[g_clicks[index].main].frame);
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
	if (g_resetQueued) {
		return;
	}
	g_resetQueued = true;
	geode::queueInMainThread([]() {
		g_resetQueued = false;
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

Click currentClick() {
	Click c = g_clicks[g_clickIndex];
	if (!g_block) {
		return c;
	}
	int p = partnerOf(g_clickIndex);
	if (p >= 0) {
		Click partner = g_clicks[p];
		for (size_t k = 0; k < partner.idx.size(); k++) {
			c.idx.push_back(partner.idx[k]);
		}
	}
	updateBoundsOf(c);
	return c;
}

void prepareTrial() {
	if (g_block) {
		Click block = currentClick();
		block.nextFrame = -1;
		g_isWave = true;
		prepareTrialFor(block);
		return;
	}
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

void writeFwc(std::string fileName, bool pressesOnly, int maxWindow) {
	std::vector<size_t> list;
	for (size_t i = 0; i < g_clicks.size(); i++) {
		if (pressesOnly && !g_clicks[i].down) {
			continue;
		}
		if (maxWindow > 0 && g_windows[i] > maxWindow) {
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

std::string modeName(int flags) {
	std::string name = "cube";
	if (flags & 2) {
		name = "ship";
	} else if (flags & 4) {
		name = "ufo";
	} else if (flags & 8) {
		name = "ball";
	} else if (flags & 16) {
		name = "wave";
	} else if (flags & 32) {
		name = "robot";
	} else if (flags & 64) {
		name = "spider";
	} else if (flags & 128) {
		name = "swing";
	}
	if (flags & 512) {
		name = "mini " + name;
	}
	return name;
}

std::string modeOf(size_t i) {
	auto input = g_original[g_clicks[i].main];
	int frame = (int)input.frame;
	if (frame < 0 || frame >= (int)g_baseline.size()) {
		return "unknown";
	}
	bool isPlayer1 = !input.player2;
	if (g_swap) {
		isPlayer1 = input.player2;
	}
	if (!isPlayer1 && g_baseline[frame].x2 != 0.0f) {
		return modeName(g_baseline[frame].flags2);
	}
	return modeName(g_baseline[frame].flags1);
}

int fpGroup(int window) {
	if (window <= 1) {
		return 0;
	}
	if (window == 2) {
		return 1;
	}
	if (window == 3) {
		return 2;
	}
	if (window == 4) {
		return 3;
	}
	if (window <= 8) {
		return 4;
	}
	return 5;
}

void writeSection(std::ofstream& txt, std::string mode, int startFrame, int endFrame, int counts[6]) {
	txt << fmt::format("{:>6.2f} - {:>6.2f} s  {:<12}", startFrame / 240.0, endFrame / 240.0, mode);
	txt << " 240fp " << counts[0] << "  120fp " << counts[1] << "  80fp " << counts[2] << "  60fp " << counts[3] << "  30fp " << counts[4] << "  easier " << counts[5] << "\n";
}

void writeResults() {
	std::ofstream txt(outDir() / (g_name + "_result.txt"), std::ios::out | std::ios::trunc);
	std::map<int, int> pressCounts;
	std::map<int, int> releaseCounts;
	for (size_t i = 0; i < g_clicks.size(); i++) {
		auto input = g_original[g_clicks[i].main];
		std::string type = g_clicks[i].down ? "press" : "release";
		txt << type << " " << i << " frame " << input.frame << " p2 " << input.player2 << " window " << g_windows[i] << " mode " << modeOf(i) << "\n";
		if (g_clicks[i].down) {
			pressCounts[g_windows[i]]++;
		} else {
			releaseCounts[g_windows[i]]++;
		}
	}

	txt << "\nsections\n";
	int total[6] = { 0, 0, 0, 0, 0, 0 };
	std::map<std::string, std::vector<int>> byMode;
	int counts[6] = { 0, 0, 0, 0, 0, 0 };
	std::string current = "";
	int startFrame = 0;
	int lastFrame = 0;
	for (size_t i = 0; i < g_clicks.size(); i++) {
		std::string mode = modeOf(i);
		int frame = (int)g_original[g_clicks[i].main].frame;
		if (mode != current) {
			if (current != "") {
				writeSection(txt, current, startFrame, lastFrame, counts);
			}
			current = mode;
			startFrame = frame;
			for (int k = 0; k < 6; k++) {
				counts[k] = 0;
			}
		}
		int g = fpGroup(g_windows[i]);
		counts[g]++;
		total[g]++;
		if (byMode[mode].empty()) {
			byMode[mode].assign(6, 0);
		}
		byMode[mode][g]++;
		lastFrame = frame;
	}
	if (current != "") {
		writeSection(txt, current, startFrame, lastFrame, counts);
	}

	txt << "\nby mode\n";
	for (auto& [mode, c] : byMode) {
		txt << fmt::format("{:<12}", mode) << " 240fp " << c[0] << "  120fp " << c[1] << "  80fp " << c[2] << "  60fp " << c[3] << "  30fp " << c[4] << "  easier " << c[5] << "\n";
	}
	txt << "\ntotal        240fp " << total[0] << "  120fp " << total[1] << "  80fp " << total[2] << "  60fp " << total[3] << "  30fp " << total[4] << "  easier " << total[5] << "\n";

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

	writeFwc(g_name + "_windows.fwc", false, 0);
	writeFwc(g_name + "_clicks.fwc", true, 0);
	writeFwc(g_name + "_clean.fwc", false, 8);
}

void finishAnalysis() {
	writeResults();
	if (g_human) {
		writeHumanMacro();
		int passes = (int)Mod::get()->getSettingValue<int64_t>("human-passes");
		g_log << "pass " << g_pass << " moves " << g_moves << "\n";
		g_log.flush();
		if (g_pass < passes && g_moves > 0) {
			g_pass++;
			g_moves = 0;
			Notification::create(fmt::format("Pass {}/{}", g_pass, passes), NotificationIcon::Info)->show();
			g_inputs = g_original;
			g_baseline.clear();
			g_collapseDone = true;
			g_mode = 1;
			requestReset();
			return;
		}
	}
	g_mode = 0;
	setSpeed(1.0f);
	Notification::create("Analysis finished! See gd-mods folder", NotificationIcon::Success)->show();
}

bool shiftAllowed() {
	Click c = currentClick();
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
	if (!g_block) {
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

void clearRecording() {
	g_tmpBase.clear();
	g_tmpStart = -1;
	g_tmpMatch.clear();
}

void commitRecording() {
	if (g_tmpStart < 0) {
		return;
	}
	for (size_t i = 0; i < g_tmpBase.size(); i++) {
		size_t t = (size_t)g_tmpStart + i;
		if (t < g_baseline.size()) {
			g_baseline[t] = g_tmpBase[i];
		} else {
			g_baseline.push_back(g_tmpBase[i]);
		}
	}
	int kept = 0;
	int lost = 0;
	for (size_t i = 0; i < g_cpTick.size() && i < g_cpOk.size(); i++) {
		if (g_cpTick[i] < g_tmpStart) {
			continue;
		}
		bool matched = g_tmpMatch.count((int)i) > 0 && g_tmpMatch[(int)i];
		if (g_cpOk[i] && !matched) {
			g_cpOk[i] = false;
			lost++;
		}
		if (g_cpOk[i]) {
			kept++;
		}
	}
	g_log << "new path from " << g_tmpStart << ", checkpoints kept " << kept << " lost " << lost << "\n";
	clearRecording();
}

void prepareVerify() {
	Click c = g_clicks[g_clickIndex];
	int p = -1;
	if (g_block) {
		p = partnerOf(g_clickIndex);
	}
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
	g_checkFrom = 1000000000;
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
		int p = -1;
		if (g_block) {
			p = partnerOf(g_clickIndex);
		}
		if (p >= 0) {
			Click partner = g_clicks[p];
			for (size_t k = 0; k < partner.idx.size(); k++) {
				g_original[partner.idx[k]].frame += g_vshift;
			}
		}
		g_moves++;
		commitRecording();
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
		Click c = currentClick();
		int minAllowed = std::max(c.minShift, -MAX_SHIFT);
		int maxAllowed = std::min(c.maxShift, MAX_SHIFT);
		if (g_lo <= minAllowed || g_hi >= maxAllowed) {
			g_log << "skip clipped window " << g_clickIndex << "\n";
			center = 0;
		}
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

bool findCollapse() {
	std::vector<Click> events = buildEvents();
	for (size_t i = 0; i + 2 < events.size(); i++) {
		Click a = events[i];
		int fa = (int)g_original[a.main].frame;
		if (fa <= g_collapseFrom) {
			continue;
		}
		if (!waveAt(fa)) {
			continue;
		}
		int j = -1;
		int k = -1;
		for (size_t m = i + 1; m < events.size(); m++) {
			if (events[m].p2 != a.p2) {
				continue;
			}
			if (j < 0) {
				j = (int)m;
			} else {
				k = (int)m;
				break;
			}
		}
		if (j < 0 || k < 0) {
			continue;
		}
		int d = -1;
		for (int m = (int)i - 1; m >= 0; m--) {
			if (events[m].p2 == a.p2) {
				d = m;
				break;
			}
		}
		Click b = events[j];
		Click c = events[k];
		if (b.down == a.down || c.down != a.down) {
			continue;
		}
		int len = (int)g_original[b.main].frame - fa;
		if (len < 1 || len > MICRO_TICKS) {
			continue;
		}
		g_cutA = a.main;
		g_cutC = c.main;
		g_cutLen = len;
		g_hasD = d >= 0 && events[d].down != a.down;
		if (g_hasD) {
			g_cutD = events[d].main;
		}
		g_variant = 1;
		g_collapseFrom = fa;
		return true;
	}
	return false;
}

std::vector<gdr::Input<>> collapsedInputs() {
	int fa = (int)g_original[g_cutA].frame;
	int fc = (int)g_original[g_cutC].frame;
	int fd = -1;
	if (g_hasD) {
		fd = (int)g_original[g_cutD].frame;
	}
	bool p2 = g_original[g_cutA].player2;
	std::vector<gdr::Input<>> result;
	for (size_t i = 0; i < g_original.size(); i++) {
		auto input = g_original[i];
		int f = (int)input.frame;
		if (input.player2 == p2) {
			if (g_variant == 1) {
				if (f >= fa && f < fc) {
					continue;
				}
				if (f == fc) {
					input.frame = fc - g_cutLen;
				}
			} else {
				if (f > fd && f < fc) {
					continue;
				}
				if (f == fd) {
					input.frame = fd + g_cutLen;
				}
			}
		}
		result.push_back(input);
	}
	std::stable_sort(result.begin(), result.end(), sortByFrame);
	return result;
}

void prepareCollapseTrial() {
	g_inputs = collapsedInputs();
	g_checkFrom = 1000000000;
	g_limit = -1;
	g_trials++;
}

void startWindowsAfterCollapse();

void collapseNext() {
	if (findCollapse()) {
		prepareCollapseTrial();
		return;
	}
	g_log << "micro clicks removed " << g_collapseCount << "\n";
	g_log.flush();
	g_collapseDone = true;
	if (g_collapseCount == 0) {
		startWindowsAfterCollapse();
		return;
	}
	Notification::create(fmt::format("Removed {} micro clicks, new baseline", g_collapseCount), NotificationIcon::Info)->show();
	g_inputs = g_original;
	g_baseline.clear();
	g_mode = 1;
	requestReset();
}

void collapseAfter(bool ok) {
	g_log << "micro click at " << g_original[g_cutA].frame << " len " << g_cutLen << " variant " << g_variant << " removed " << ok << "\n";
	if (!ok && g_variant == 1 && g_hasD) {
		g_variant = 2;
		prepareCollapseTrial();
		return;
	}
	if (ok) {
		g_original = collapsedInputs();
		g_collapseCount++;
		commitRecording();
	}
	collapseNext();
}

int firstChange() {
	size_t n = std::min(g_inputs.size(), g_original.size());
	for (size_t i = 0; i < n; i++) {
		auto a = g_inputs[i];
		auto b = g_original[i];
		if (a.frame != b.frame || a.down != b.down || a.player2 != b.player2) {
			return (int)std::min(a.frame, b.frame);
		}
	}
	if (g_inputs.size() != g_original.size()) {
		if (n < g_inputs.size()) {
			return (int)g_inputs[n].frame;
		}
		if (n < g_original.size()) {
			return (int)g_original[n].frame;
		}
	}
	return 1000000000;
}

int pickCheckpoint() {
	int change = firstChange();
	int best = -1;
	for (size_t i = 0; i < g_cpTick.size() && i < g_cpOk.size(); i++) {
		if (g_cpOk[i] && g_cpTick[i] <= change - 2) {
			best = (int)i;
		}
	}
	return best;
}

void clearCheckpoints() {
	if (g_cpArray) {
		g_cpArray->release();
		g_cpArray = nullptr;
	}
	g_cpTick.clear();
	g_cpOk.clear();
	g_loadCp = -1;
	g_held[0] = false;
	g_held[1] = false;
	g_nextCpTick = CP_EVERY / 2;
	g_lastInputTick = -100;
}

void prepareCpTest() {
	g_inputs = g_original;
	g_loadCp = (int)g_cpTest;
	g_cpFirstTick = -1;
	g_limit = -1;
	g_checkFrom = 1000000000;
}

void startTrials();

void startCpTest() {
	g_cpOk.assign(g_cpTick.size(), false);
	g_log << "checkpoints " << g_cpTick.size() << "\n";
	g_log.flush();
	if (g_cpTick.empty()) {
		startTrials();
		return;
	}
	g_mode = 2;
	g_phase = -2;
	g_cpTest = 0;
	prepareCpTest();
	requestReset();
}

void cpTestAfter(bool ok) {
	g_cpOk[g_cpTest] = ok;
	g_log << "cp test " << g_cpTest << " tick " << g_cpTick[g_cpTest] << " ok " << ok << "\n";
	g_cpTest++;
	if (g_cpTest < g_cpTick.size()) {
		prepareCpTest();
		return;
	}
	int good = 0;
	for (size_t i = 0; i < g_cpOk.size(); i++) {
		if (g_cpOk[i]) {
			good++;
		}
	}
	g_log << "checkpoints ok " << good << " of " << g_cpOk.size() << "\n";
	g_log.flush();
	Notification::create(fmt::format("Checkpoints OK: {}/{}", good, g_cpOk.size()), NotificationIcon::Info)->show();
	g_loadCp = -1;
	startTrials();
}

void finishTrial(bool ok) {
	if (g_trialDone) {
		return;
	}
	g_trialDone = true;
	if (g_phase == -2) {
		cpTestAfter(ok);
	} else if (g_phase == -1) {
		collapseAfter(ok);
	} else if (g_phase == 0) {
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
	if (g_human && !g_collapseDone) {
		g_mode = 2;
		g_phase = -1;
		g_collapseCount = 0;
		g_collapseFrom = -1;
		collapseNext();
		requestReset();
		return;
	}
	startWindowsAfterCollapse();
}

void startWindowsAfterCollapse() {
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
		startCpTest();
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
		g_collapseDone = false;
		clearCheckpoints();
		g_pass = 1;
		g_moves = 0;
		std::string text = fmt::format("Loaded {} inputs.\nOpen the level in normal mode\nand don't touch anything.", g_original.size());
		FLAlertLayer::create("FW Analyzer", text, "OK")->show();
	}
};

class $modify(MyBaseLayer, GJBaseGameLayer) {
	void processQueuedButtons(float dt, bool clearInputQueue) {
		if (g_mode != 0 && PlayLayer::get() && g_waitStart) {
			if (m_gameState.m_currentProgress / 2 <= 2) {
				g_waitStart = false;
			}
		}
		if (g_mode != 0 && PlayLayer::get() && !g_trialDone && !g_waitStart && g_findOffset) {
			int raw = m_gameState.m_currentProgress / 2;
			State first = readState(this);
			g_findOffset = false;
			g_tickOffset = 0;
			bool found = false;
			for (int d = -3; d <= 3; d++) {
				int t = raw + d;
				if (t >= 0 && t < (int)g_baseline.size() && sameState(first, g_baseline[t])) {
					g_tickOffset = d;
					found = true;
					break;
				}
			}
			if (!found) {
				g_tickOffset = -1;
				g_log << "offset not found after cp " << g_loadCp << " raw " << raw << "\n";
				if (g_phase != -2 && g_loadCp >= 0 && g_loadCp < (int)g_cpOk.size()) {
					g_cpOk[g_loadCp] = false;
					g_trialDone = true;
					requestReset();
				}
			}
			if (g_phase == -2) {
				g_log << "offset after cp raw " << raw << " offset " << g_tickOffset << "\n";
			}
		}
		if (g_mode != 0 && PlayLayer::get() && !g_trialDone && !g_waitStart) {
			int tick = m_gameState.m_currentProgress / 2 + g_tickOffset;
			while (g_next < g_inputs.size() && (int)g_inputs[g_next].frame <= tick) {
				auto input = g_inputs[g_next];
				bool isPlayer1 = !input.player2;
				if (g_swap) {
					isPlayer1 = input.player2;
				}
				this->handleButton(input.down, input.button, isPlayer1);
				if (g_mode == 1) {
					g_held[input.player2 ? 1 : 0] = input.down;
					g_lastInputTick = tick;
				}
				g_next++;
			}

			State now = readState(this);
			auto pl = PlayLayer::get();
			if (g_mode == 1) {
				while ((int)g_baseline.size() <= tick) {
					g_baseline.push_back(now);
				}
				int nextInput = 1000000000;
				if (g_next < g_inputs.size()) {
					nextInput = (int)g_inputs[g_next].frame;
				}
				if (tick >= g_nextCpTick && !g_held[0] && !g_held[1] && tick - g_lastInputTick >= 2 && nextInput - tick >= 2) {
					auto cp = pl->createCheckpoint();
					if (cp) {
						if (!g_cpArray) {
							g_cpArray = cocos2d::CCArray::create();
							g_cpArray->retain();
						}
						g_cpArray->addObject(cp);
						g_cpTick.push_back(tick);
						g_nextCpTick = tick + CP_EVERY;
					}
				}
			}
			if (g_mode == 2 && (g_verifying || g_phase == -1) && !g_trialDone) {
				if (g_tmpStart < 0) {
					g_tmpStart = tick;
				}
				while ((int)g_tmpBase.size() <= tick - g_tmpStart) {
					g_tmpBase.push_back(now);
				}
				for (size_t k = 0; k < g_cpTick.size(); k++) {
					if (g_cpTick[k] == tick && tick < (int)g_baseline.size()) {
						g_tmpMatch[(int)k] = sameState(now, g_baseline[tick]);
					}
				}
			}
			if (g_mode == 2 && g_phase == -2 && !g_trialDone) {
				int start = g_cpTick[g_cpTest];
				if (g_cpFirstTick < 0) {
					g_cpFirstTick = tick;
					bool same = tick < (int)g_baseline.size() && sameState(now, g_baseline[tick]);
					g_log << "cp " << g_cpTest << " start " << start << " first tick " << tick << " same " << same << " x " << now.x1 << " y " << now.y1 << "\n";
				}
				if (tick > start && (tick - start) % CP_TEST_TICKS == 0) {
					bool ok = tick < (int)g_baseline.size() && sameState(now, g_baseline[tick]);
					if (!ok) {
						g_log << "cp " << g_cpTest << " differs at " << tick << "\n";
						finishTrial(false);
					}
				}
			}
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
		g_waitStart = true;
		if (g_mode == 1) {
			clearCheckpoints();
		}
		if (g_mode == 2 && g_phase != -2) {
			g_loadCp = -1;
			if (Mod::get()->getSettingValue<bool>("use-checkpoints")) {
				g_loadCp = pickCheckpoint();
			}
		}
		bool useCp = g_mode == 2 && g_loadCp >= 0 && g_cpArray && g_loadCp < (int)g_cpArray->count();
		if (g_mode != 0 && m_checkpointArray) {
			m_checkpointArray->removeAllObjects();
			m_isPracticeMode = false;
			if (useCp) {
				auto cp = static_cast<CheckpointObject*>(g_cpArray->objectAtIndex(g_loadCp));
				m_checkpointArray->addObject(cp);
				m_isPracticeMode = true;
			}
		}
		PlayLayer::resetLevel();
		if (g_mode != 0 && m_checkpointArray) {
			m_checkpointArray->removeAllObjects();
			m_isPracticeMode = false;
		}
		if (g_mode == 2) {
			setSpeed((float)Mod::get()->getSettingValue<double>("speed"));
		}

		g_next = 0;
		g_trialDone = false;
		g_tickOffset = 0;
		clearRecording();
		g_findOffset = false;
		if (useCp) {
			g_findOffset = true;
			int start = g_cpTick[g_loadCp];
			while (g_next < g_inputs.size() && (int)g_inputs[g_next].frame <= start) {
				g_next++;
			}
			g_waitStart = false;
		}
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
		clearCheckpoints();
		clearRecording();
		if (m_checkpointArray) {
			m_checkpointArray->removeAllObjects();
		}
		g_mode = 0;
		setSpeed(1.0f);
		PlayLayer::onQuit();
	}
};
