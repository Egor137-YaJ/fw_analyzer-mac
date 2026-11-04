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
	size_t press;
	size_t release;
	bool hasRelease;
	int minShift;
	int maxShift;
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
const float SPEED = 20.0f;

std::vector<gdr::Input<>> g_original;
std::vector<gdr::Input<>> g_inputs;
size_t g_next = 0;
int g_mode = 0;
bool g_trialDone = false;
int g_checkFrom = 0;

std::vector<Click> g_clicks;
std::vector<int> g_windows;
std::vector<State> g_baseline;
size_t g_clickIndex = 0;
int g_shift = 0;
int g_dir = -1;
int g_lo = 0;
int g_hi = 0;
int g_trials = 0;

std::ofstream g_log;

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
	auto path = Mod::get()->getSaveDir().parent_path() / "eclipse.eclipse-menu" / "replays" / "startfa1.gdr2";
	auto res = Replay::importData(path);
	if (res.isErr()) {
		FLAlertLayer::create("FW Analyzer", "Can't read replay: " + res.unwrapErr() + "\n" + path.string(), "OK")->show();
		return false;
	}
	g_original = res.unwrap().inputs;
	std::stable_sort(g_original.begin(), g_original.end(), sortByFrame);
	return true;
}

void buildClicks() {
	g_clicks.clear();
	for (size_t i = 0; i < g_original.size(); i++) {
		if (!g_original[i].down) {
			continue;
		}
		Click c;
		c.press = i;
		c.release = 0;
		c.hasRelease = false;
		for (size_t j = i + 1; j < g_original.size(); j++) {
			if (g_original[j].player2 == g_original[i].player2 && !g_original[j].down) {
				c.release = j;
				c.hasRelease = true;
				break;
			}
		}

		int prevFrame = 0;
		for (size_t j = 0; j < i; j++) {
			if (g_original[j].player2 == g_original[i].player2) {
				prevFrame = (int)g_original[j].frame;
			}
		}

		size_t last = c.hasRelease ? c.release : c.press;
		int nextFrame = -1;
		for (size_t j = last + 1; j < g_original.size(); j++) {
			if (g_original[j].player2 == g_original[i].player2) {
				nextFrame = (int)g_original[j].frame;
				break;
			}
		}

		int pressFrame = (int)g_original[c.press].frame;
		int lastFrame = (int)g_original[last].frame;
		c.minShift = prevFrame - pressFrame + 1;
		if (pressFrame + c.minShift < 1) {
			c.minShift = 1 - pressFrame;
		}
		c.maxShift = MAX_SHIFT;
		if (nextFrame >= 0) {
			c.maxShift = nextFrame - lastFrame - 1;
		}
		g_clicks.push_back(c);
	}
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

void prepareTrial() {
	Click c = g_clicks[g_clickIndex];
	g_inputs = g_original;
	g_inputs[c.press].frame += g_shift;
	int lastOriginal = (int)g_original[c.press].frame;
	if (c.hasRelease) {
		g_inputs[c.release].frame += g_shift;
		lastOriginal = (int)g_original[c.release].frame;
	}
	std::stable_sort(g_inputs.begin(), g_inputs.end(), sortByFrame);
	g_checkFrom = std::max(lastOriginal, lastOriginal + g_shift) + 2;
	g_trials++;
}

void writeResults() {
	std::ofstream txt(outDir() / "fwa_result.txt", std::ios::out | std::ios::trunc);
	std::map<int, int> counts;
	for (size_t i = 0; i < g_clicks.size(); i++) {
		auto input = g_original[g_clicks[i].press];
		txt << "click " << i << " frame " << input.frame << " p2 " << input.player2 << " window " << g_windows[i] << "\n";
		counts[g_windows[i]]++;
	}
	txt << "\n";
	for (auto& [w, n] : counts) {
		txt << "window " << w << ": " << n << "\n";
	}

	std::ofstream fwc(outDir() / "starfa_windows.fwc", std::ios::out | std::ios::binary | std::ios::trunc);
	fwc.write("FWC2", 4);
	double fps = 240.0;
	fwc.write(reinterpret_cast<const char*>(&fps), sizeof(double));
	uint32_t count = (uint32_t)g_clicks.size();
	fwc.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
	for (size_t i = 0; i < g_clicks.size(); i++) {
		auto input = g_original[g_clicks[i].press];
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

void nextClickOrFinish() {
	g_windows.push_back(g_hi - g_lo + 1);
	g_log << "click " << g_clickIndex << " frame " << g_original[g_clicks[g_clickIndex].press].frame << " lo " << g_lo << " hi " << g_hi << " window " << (g_hi - g_lo + 1) << "\n";
	g_log.flush();
	g_clickIndex++;
	if (g_clickIndex % 10 == 0 && g_clickIndex < g_clicks.size()) {
		Notification::create(fmt::format("Click {}/{}", g_clickIndex, g_clicks.size()), NotificationIcon::Info)->show();
	}
	g_dir = -1;
	g_shift = -1;
	g_lo = 0;
	g_hi = 0;
	if (g_clickIndex >= g_clicks.size()) {
		g_mode = 0;
		setSpeed(1.0f);
		writeResults();
		Notification::create("Analysis finished! See gd-mods folder", NotificationIcon::Success)->show();
	}
}

bool shiftAllowed() {
	Click c = g_clicks[g_clickIndex];
	return g_shift >= c.minShift && g_shift <= c.maxShift && g_shift >= -MAX_SHIFT && g_shift <= MAX_SHIFT;
}

void seekValid() {
	while (g_mode == 2 && !shiftAllowed()) {
		if (g_dir == -1) {
			g_dir = 1;
			g_shift = 1;
		} else {
			nextClickOrFinish();
		}
	}
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
			nextClickOrFinish();
		}
	}
	seekValid();
	if (g_mode == 2) {
		prepareTrial();
	}
}

void finishTrial(bool ok) {
	if (g_trialDone) {
		return;
	}
	g_trialDone = true;
	advanceAfter(ok);
	if (g_mode == 2) {
		requestReset();
	}
}

void startTrials() {
	buildClicks();
	g_windows.clear();
	g_clickIndex = 0;
	g_dir = -1;
	g_shift = -1;
	g_lo = 0;
	g_hi = 0;
	g_trials = 0;
	g_mode = 2;
	g_log << "baseline ticks " << g_baseline.size() << " clicks " << g_clicks.size() << "\n";
	g_log.flush();
	if (g_clicks.empty()) {
		g_mode = 0;
		return;
	}
	seekValid();
	if (g_mode == 2) {
		prepareTrial();
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
		setSpeed(SPEED);
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
				this->handleButton(input.down, input.button, input.player2);
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
			if (!g_trialDone && g_mode == 2 && tick >= g_checkFrom && tick < (int)g_baseline.size()) {
				if (sameState(now, g_baseline[tick])) {
					g_log << "trial click " << g_clickIndex << " shift " << g_shift << " ok (same state at " << tick << ")\n";
					finishTrial(true);
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
