#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <fstream>
#include <cstring>
#include <filesystem>

using namespace geode::prelude;

// --- Глобальные переменные ---
struct InputFrame { int frame; int button; bool down; bool player2; };
static std::vector<InputFrame> g_recordedInputs;
static int g_currentFrame = 0;
static bool g_isRecording = false;
static bool g_isPlaying = false;
static size_t g_playbackIndex = 0;
static CCLabelBMFont* g_statusLabel = nullptr;
static std::string g_loadedMacroName = "";

static CCMenuItemSpriteExtra* g_stepButton = nullptr;
static CCLabelBMFont* g_stepLabel = nullptr;
static bool g_frameStepperEnabled = false;
static bool g_stepperAdvance = false;

static double g_speedhackValue = 1.0;
static double g_tpsValue = 60.0;
static bool g_lockDelta = false;
static bool g_useVisualUpdates = false;
static float g_lockedDelta = 1.0f / 240.0f;

// ==========================================
// Формат .rle v2
// ==========================================

static void writeVarint(std::ofstream& f, uint32_t value) {
    while (value >= 0x80) {
        f.put(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    f.put(static_cast<char>(value & 0x7F));
}

static uint32_t readVarint(std::ifstream& f) {
    uint32_t result = 0;
    int shift = 0;
    char byte;
    while (f.get(byte)) {
        uint8_t b = static_cast<uint8_t>(byte);
        result |= static_cast<uint32_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 28) break;
    }
    return result;
}

static std::filesystem::path getReplayDir() {
    return Mod::get()->getSaveDir();
}

static std::filesystem::path getReplayPath() {
    return getReplayDir() / "replay.rle";
}

static void saveReplayAs(const std::string& name) {
    std::filesystem::path path = getReplayDir() / (name + ".rle");
    std::ofstream file(path, std::ios::binary);
    if (!file) return;

    const char magic[4] = {'R', 'L', 'E', '\0'};
    file.write(magic, 4);
    file.put(static_cast<char>(2));
    writeVarint(file, static_cast<uint32_t>(g_recordedInputs.size()));

    int prevFrame = 0;
    for (auto& input : g_recordedInputs) {
        uint32_t delta = static_cast<uint32_t>(input.frame - prevFrame);
        writeVarint(file, delta);
        prevFrame = input.frame;
    }

    uint8_t buffer = 0;
    int bitsFilled = 0;
    for (auto& input : g_recordedInputs) {
        uint8_t action = (input.button & 0x03);
        if (input.down)    action |= 0x04;
        if (input.player2) action |= 0x08;

        buffer |= static_cast<uint8_t>(action << bitsFilled);
        bitsFilled += 4;
        if (bitsFilled == 8) {
            file.put(static_cast<char>(buffer));
            buffer = 0;
            bitsFilled = 0;
        }
    }
    if (bitsFilled > 0) file.put(static_cast<char>(buffer));
}

static bool loadReplayFrom(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    char magic[4];
    file.read(magic, 4);
    if (std::memcmp(magic, "RLE", 3) != 0) return false;

    char versionByte;
    file.get(versionByte);
    if (static_cast<uint8_t>(versionByte) != 2) return false;

    uint32_t eventCount = readVarint(file);
    g_recordedInputs.clear();
    g_recordedInputs.reserve(eventCount);

    std::vector<int> frames(eventCount);
    int currentFrame = 0;
    for (uint32_t i = 0; i < eventCount; i++) {
        currentFrame += static_cast<int>(readVarint(file));
        frames[i] = currentFrame;
    }

    uint8_t buffer = 0;
    int bitsAvailable = 0;
    for (uint32_t i = 0; i < eventCount; i++) {
        if (bitsAvailable < 4) {
            char b;
            file.get(b);
            buffer = static_cast<uint8_t>(b);
            bitsAvailable = 8;
        }
        uint8_t action = buffer & 0x0F;
        buffer >>= 4;
        bitsAvailable -= 4;

        InputFrame input;
        input.frame = frames[i];
        input.button = action & 0x03;
        input.down = (action & 0x04) != 0;
        input.player2 = (action & 0x08) != 0;
        g_recordedInputs.push_back(input);
    }
    return true;
}

static void updateStatusLabel() {
    if (!g_statusLabel) return;
    if (!g_statusLabel->getParent()) { g_statusLabel = nullptr; return; }

    if (g_isRecording) {
        g_statusLabel->setString(fmt::format("REC: {}", g_currentFrame).c_str());
        g_statusLabel->setColor({255, 100, 100});
    } else if (g_isPlaying) {
        g_statusLabel->setString("PLAYING...");
        g_statusLabel->setColor({100, 255, 100});
    } else {
        if (!g_loadedMacroName.empty()) {
            g_statusLabel->setString(g_loadedMacroName.c_str());
        } else {
            g_statusLabel->setString(fmt::format("Inputs: {}", g_recordedInputs.size()).c_str());
        }
        g_statusLabel->setColor({255, 255, 255});
    }
}

// ==========================================
// Speedhack / TPS / Lock Delta / Frame Stepper
// ==========================================
class $modify(MyScheduler, cocos2d::CCScheduler) {
    void update(float dt) {
        auto playLayer = PlayLayer::get();
        if (!playLayer || playLayer->m_isPaused) {
            cocos2d::CCScheduler::update(dt);
            return;
        }

        if (g_frameStepperEnabled) return;

        float effectiveDt = dt;
        if (g_lockDelta) effectiveDt = g_lockedDelta;
        if (!g_lockDelta) effectiveDt *= static_cast<float>(g_speedhackValue);
        effectiveDt *= static_cast<float>(g_tpsValue / 60.0);

        cocos2d::CCScheduler::update(effectiveDt);
    }
};

// ==========================================
// Игрок (воспроизведение)
// ==========================================
class $modify(MyPlayerObject, PlayerObject) {
    void update(float dt) {
        if (g_isPlaying && !g_recordedInputs.empty()) {
            while (g_playbackIndex < g_recordedInputs.size() &&
                   g_recordedInputs[g_playbackIndex].frame <= g_currentFrame) {
                auto& input = g_recordedInputs[g_playbackIndex];
                bool isP2 = (PlayLayer::get() && PlayLayer::get()->m_player2 == this);
                if (input.player2 == isP2) {
                    int btn = input.button;
                    if (btn >= 0 && btn < 3) {
                        m_holdingButtons[btn] = input.down;
                    }
                }
                g_playbackIndex++;
            }
        }
        PlayerObject::update(dt);
    }
};

// ==========================================
// Запись
// ==========================================
class $modify(MyGJBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool player2) {
        if (g_isRecording && !g_isPlaying && PlayLayer::get()) {
            g_recordedInputs.push_back({g_currentFrame, button, down, player2});
        }
        GJBaseGameLayer::handleButton(down, button, player2);
    }
};

// ==========================================
// PlayLayer: кнопки в левом нижнем углу
// ==========================================
class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        CCMenuItemSpriteExtra* m_stepBtn = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto winSize = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 100);

        auto pauseSpr = CCSprite::createWithSpriteFrameName("GJ_pauseBtn_001.png");
        if (!pauseSpr) pauseSpr = CCSprite::createWithSpriteFrameName("pauseButton_001.png");
        if (pauseSpr) {
            pauseSpr->setScale(0.8f);
            g_stepButton = CCMenuItemSpriteExtra::create(
                pauseSpr, this, menu_selector(MyPlayLayer::onStepToggle));
            g_stepButton->setPosition({30.f, 30.f});
            menu->addChild(g_stepButton);
        }

        auto stepSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        if (!stepSpr) stepSpr = CCSprite::createWithSpriteFrameName("edit_arrow.png");
        if (stepSpr) {
            stepSpr->setScale(0.8f);
            m_fields->m_stepBtn = CCMenuItemSpriteExtra::create(
                stepSpr, this, menu_selector(MyPlayLayer::onStepAdvance));
            m_fields->m_stepBtn->setPosition({70.f, 30.f});
            menu->addChild(m_fields->m_stepBtn);
        }

        g_stepLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_stepLabel->setScale(0.3f);
        g_stepLabel->setPosition({30.f, 60.f});
        menu->addChild(g_stepLabel);

        return true;
    }

    void onStepToggle(CCObject*) {
        g_frameStepperEnabled = !g_frameStepperEnabled;

        if (g_stepButton) {
            CCSprite* newSpr = nullptr;
            if (g_frameStepperEnabled) {
                newSpr = CCSprite::createWithSpriteFrameName("GJ_playBtn_001.png");
                if (!newSpr) newSpr = CCSprite::createWithSpriteFrameName("playButton_001.png");
            } else {
                newSpr = CCSprite::createWithSpriteFrameName("GJ_pauseBtn_001.png");
                if (!newSpr) newSpr = CCSprite::createWithSpriteFrameName("pauseButton_001.png");
            }
            if (newSpr) {
                newSpr->setScale(0.8f);
                g_stepButton->setSprite(newSpr);
            }
        }

        if (g_stepLabel) {
            g_stepLabel->setString(g_frameStepperEnabled ? "PAUSED" : "");
        }
    }

    void onStepAdvance(CCObject*) {
        g_stepperAdvance = true;
    }

    void update(float dt) {
        PlayLayer::update(dt);
        if (g_isRecording) { g_currentFrame++; updateStatusLabel(); }

        if (g_frameStepperEnabled && g_stepperAdvance) {
            g_stepperAdvance = false;
            g_currentFrame++;
            PlayLayer::update(1.0f / 60.0f);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_currentFrame = 0;
        g_playbackIndex = 0;
    }
};

// ==========================================
// Меню паузы
// ==========================================
class $modify(MyPauseLayer, PauseLayer) {
    struct Fields {
        CCMenu* m_subMenu = nullptr;
        bool m_expanded = false;
    };

    void customSetup() {
        PauseLayer::customSetup();
        auto winSize = CCDirector::get()->getWinSize();
        auto mainMenu = CCMenu::create(); mainMenu->setPosition({0, 0}); this->addChild(mainMenu);

        auto botBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("BOT", "bigFont.fnt", "GJ_button_01.png"),
            this, menu_selector(MyPauseLayer::onToggle));
        botBtn->setPosition({winSize.width - 60.f, 60.f}); mainMenu->addChild(botBtn);

        m_fields->m_subMenu = CCMenu::create(); m_fields->m_subMenu->setPosition({0, 0});
        m_fields->m_subMenu->setVisible(false); this->addChild(m_fields->m_subMenu);

        auto recBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("REC", "bigFont.fnt", "GJ_button_01.png"),
            this, menu_selector(MyPauseLayer::onRecord));
        recBtn->setPosition({winSize.width - 60.f, 110.f}); m_fields->m_subMenu->addChild(recBtn);

        auto playBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("PLAY", "bigFont.fnt", "GJ_button_02.png"),
            this, menu_selector(MyPauseLayer::onPlay));
        playBtn->setPosition({winSize.width - 60.f, 150.f}); m_fields->m_subMenu->addChild(playBtn);

        auto saveBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("SAVE", "bigFont.fnt", "GJ_button_03.png"),
            this, menu_selector(MyPauseLayer::onSave));
        saveBtn->setPosition({winSize.width - 60.f, 190.f}); m_fields->m_subMenu->addChild(saveBtn);

        auto searchBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("SEARCH", "bigFont.fnt", "GJ_button_04.png"),
            this, menu_selector(MyPauseLayer::onSearch));
        searchBtn->setPosition({winSize.width - 60.f, 230.f}); m_fields->m_subMenu->addChild(searchBtn);

        g_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_statusLabel->setScale(0.4f); g_statusLabel->setPosition({winSize.width - 60.f, 95.f});
        mainMenu->addChild(g_statusLabel); updateStatusLabel();
    }

    void onToggle(CCObject*) { m_fields->m_expanded = !m_fields->m_expanded; m_fields->m_subMenu->setVisible(m_fields->m_expanded); }
    void onRecord(CCObject*) {
        if (g_isPlaying) g_isPlaying = false;
        g_isRecording = !g_isRecording;
        if (g_isRecording) { g_recordedInputs.clear(); g_currentFrame = 0; g_loadedMacroName = ""; }
        updateStatusLabel();
    }
    void onPlay(CCObject*) {
        if (g_isRecording) g_isRecording = false;
        if (g_recordedInputs.empty()) { FLAlertLayer::create("RLE Bot", "No inputs recorded!", "OK")->show(); return; }
        g_isPlaying = !g_isPlaying;
        if (g_isPlaying) { g_currentFrame = 0; g_playbackIndex = 0; }
        updateStatusLabel();
    }
    void onSave(CCObject*) {
        if (g_recordedInputs.empty()) { FLAlertLayer::create("RLE Bot", "Nothing to save!", "OK")->show(); return; }
        saveReplayAs("macro");
        FLAlertLayer::create("RLE Bot", "Saved to macro.rle", "OK")->show();
        updateStatusLabel();
    }
    void onSearch(CCObject*) {
        std::vector<std::string> macroNames;
        auto dir = getReplayDir();
        if (std::filesystem::exists(dir)) {
            for (auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.path().extension() == ".rle") {
                    macroNames.push_back(entry.path().stem().string());
                }
            }
        }
        if (macroNames.empty()) { FLAlertLayer::create("Search", "No macros found.", "OK")->show(); return; }
        std::string firstName = macroNames[0];
        auto fullPath = dir / (firstName + ".rle");
        if (loadReplayFrom(fullPath)) {
            g_loadedMacroName = firstName;
            FLAlertLayer::create("Search", ("Loaded: " + firstName).c_str(), "OK")->show();
        } else {
            FLAlertLayer::create("Search", "Failed to load.", "OK")->show();
        }
        updateStatusLabel();
    }
};

// ==========================================
// Инициализация настроек
// ==========================================
$execute {
    listenForSettingChanges<double>("speedhack-value", [](double value) {
        g_speedhackValue = value;
    });
    listenForSettingChanges<double>("tps-value", [](double value) {
        g_tpsValue = value;
    });
    g_speedhackValue = Mod::get()->getSettingValue<double>("speedhack-value");
    g_tpsValue = Mod::get()->getSettingValue<double>("tps-value");
}
