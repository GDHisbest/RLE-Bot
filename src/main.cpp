#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fstream>
#include <cstring>

using namespace geode::prelude;

struct InputFrame {
    int frame;
    int button;
    bool down;
    bool player2;
};

static std::vector<InputFrame> g_recordedInputs;
static int g_currentFrame = 0;
static bool g_isRecording = false;
static bool g_isPlaying = false;
static size_t g_playbackIndex = 0;
static CCLabelBMFont* g_statusLabel = nullptr;

// ==========================================
// Формат .rle v2
// ==========================================
// "RLE\0"        — магия (4 байта)
// version = 2    — 1 байт
// eventCount     — varint
// delta stream   — eventCount varint'ов
// action stream  — ceil(eventCount × 4 / 8) байт
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

static std::filesystem::path getReplayPath() {
    return Mod::get()->getSaveDir() / "replay.rle";
}

static void saveReplay() {
    std::ofstream file(getReplayPath(), std::ios::binary);
    if (!file) {
        log::error("Failed to open file for writing");
        return;
    }

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

    file.close();
    log::info("Replay saved: {} events, {} bytes",
              g_recordedInputs.size(),
              std::filesystem::file_size(getReplayPath()));
}

static bool loadReplay() {
    std::ifstream file(getReplayPath(), std::ios::binary);
    if (!file) {
        log::warn("No replay file found");
        return false;
    }

    char magic[4];
    file.read(magic, 4);
    if (std::memcmp(magic, "RLE", 3) != 0) {
        log::error("Invalid .rle file: bad magic");
        return false;
    }

    char versionByte;
    file.get(versionByte);
    uint8_t version = static_cast<uint8_t>(versionByte);
    if (version != 2) {
        log::error("Unsupported .rle version: {}", version);
        return false;
    }

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

    log::info("Replay loaded: {} events, {} frames", eventCount, currentFrame);
    return !g_recordedInputs.empty();
}

static void updateStatusLabel() {
    if (!g_statusLabel) return;
    if (!g_statusLabel->getParent()) {
        g_statusLabel = nullptr;
        return;
    }

    if (g_isRecording) {
        g_statusLabel->setString(fmt::format("REC: {}", g_currentFrame).c_str());
        g_statusLabel->setColor({255, 100, 100});
    } else if (g_isPlaying) {
        g_statusLabel->setString("PLAYING...");
        g_statusLabel->setColor({100, 255, 100});
    } else {
        g_statusLabel->setString(fmt::format("Inputs: {}", g_recordedInputs.size()).c_str());
        g_statusLabel->setColor({255, 255, 255});
    }
}

class $modify(MyPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        // Воспроизводим ввод ТОЛЬКО во время update игрока
        if (g_isPlaying && !g_recordedInputs.empty()) {
            while (g_playbackIndex < g_recordedInputs.size() &&
                   g_recordedInputs[g_playbackIndex].frame <= g_currentFrame) {
                auto& input = g_recordedInputs[g_playbackIndex];

                // Проверяем, что это наш игрок (P1 или P2)
                bool isP2 = (PlayLayer::get() && PlayLayer::get()->m_player2 == this);
                if (input.player2 == isP2) {
                    if (input.down) {
                        this->pushButton(static_cast<PlayerButton>(input.button));
                    } else {
                        this->releaseButton(static_cast<PlayerButton>(input.button));
                    }
                }
                g_playbackIndex++;
            }
        }
    }
};

class $modify(MyGJBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool player2) {
        if (g_isRecording && !g_isPlaying && PlayLayer::get()) {
            g_recordedInputs.push_back({g_currentFrame, button, down, player2});
        }
        GJBaseGameLayer::handleButton(down, button, player2);
    }
};

class $modify(MyPlayLayer, PlayLayer) {
    void update(float dt) {
        PlayLayer::update(dt);
        if (g_isRecording) {
            g_currentFrame++;
            updateStatusLabel();
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_currentFrame = 0;
        g_playbackIndex = 0;
    }
};

class $modify(MyPauseLayer, PauseLayer) {
    struct Fields {
        CCMenu* m_subMenu = nullptr;
        bool m_expanded = false;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::get()->getWinSize();

        auto mainMenu = CCMenu::create();
        mainMenu->setPosition({0, 0});
        this->addChild(mainMenu);

        auto botBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("BOT", "bigFont.fnt", "GJ_button_01.png"),
            this, menu_selector(MyPauseLayer::onToggle));
        botBtn->setPosition({winSize.width - 60.f, 60.f});
        mainMenu->addChild(botBtn);

        m_fields->m_subMenu = CCMenu::create();
        m_fields->m_subMenu->setPosition({0, 0});
        m_fields->m_subMenu->setVisible(false);
        this->addChild(m_fields->m_subMenu);

        auto recBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("REC", "bigFont.fnt", "GJ_button_01.png"),
            this, menu_selector(MyPauseLayer::onRecord));
        recBtn->setPosition({winSize.width - 60.f, 110.f});
        m_fields->m_subMenu->addChild(recBtn);

        auto playBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("PLAY", "bigFont.fnt", "GJ_button_02.png"),
            this, menu_selector(MyPauseLayer::onPlay));
        playBtn->setPosition({winSize.width - 60.f, 150.f});
        m_fields->m_subMenu->addChild(playBtn);

        auto saveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("SAVE", "bigFont.fnt", "GJ_button_03.png"),
            this, menu_selector(MyPauseLayer::onSave));
        saveBtn->setPosition({winSize.width - 60.f, 190.f});
        m_fields->m_subMenu->addChild(saveBtn);

        auto loadBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("LOAD", "bigFont.fnt", "GJ_button_04.png"),
            this, menu_selector(MyPauseLayer::onLoad));
        loadBtn->setPosition({winSize.width - 60.f, 230.f});
        m_fields->m_subMenu->addChild(loadBtn);

        g_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_statusLabel->setScale(0.4f);
        g_statusLabel->setPosition({winSize.width - 60.f, 95.f});
        mainMenu->addChild(g_statusLabel);
        updateStatusLabel();
    }

    void onToggle(CCObject*) {
        m_fields->m_expanded = !m_fields->m_expanded;
        m_fields->m_subMenu->setVisible(m_fields->m_expanded);
    }

    void onRecord(CCObject*) {
        if (g_isPlaying) g_isPlaying = false;
        g_isRecording = !g_isRecording;
        if (g_isRecording) {
            g_recordedInputs.clear();
            g_currentFrame = 0;
        }
        updateStatusLabel();
    }

    void onPlay(CCObject*) {
        if (g_isRecording) g_isRecording = false;
        if (g_recordedInputs.empty()) {
            FLAlertLayer::create("RLE Bot", "No inputs recorded!", "OK")->show();
            return;
        }
        g_isPlaying = !g_isPlaying;
        if (g_isPlaying) {
            g_currentFrame = 0;
            g_playbackIndex = 0;
        }
        updateStatusLabel();
    }

    void onSave(CCObject*) {
        if (g_recordedInputs.empty()) {
            FLAlertLayer::create("RLE Bot", "Nothing to save!", "OK")->show();
            return;
        }
        saveReplay();
        FLAlertLayer::create("RLE Bot", "Replay saved to replay.rle", "OK")->show();
    }

    void onLoad(CCObject*) {
        if (loadReplay()) {
            FLAlertLayer::create("RLE Bot",
                fmt::format("Loaded {} events", g_recordedInputs.size()).c_str(), "OK")->show();
        } else {
            FLAlertLayer::create("RLE Bot", "Failed to load replay.rle", "OK")->show();
        }
        updateStatusLabel();
    }
};
