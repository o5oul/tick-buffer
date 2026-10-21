// Tick Buffer v1.1 - Geode mod for Geometry Dash 2.208x
// Author: o5oul
//
// Modules (each toggleable in the menu):
//   1. Orb/ring buffer     - early + late click window around orbs/rings.
//   2. Ground buffer       - early (pre-landing) and late (coyote) jumps for
//                            cube / ball / robot / spider.
//   3. Wave lookahead      - measures the wave's per-tick motion, simulates both
//                            "hold" and "release" paths against hazard/solid
//                            hitboxes for N ticks, and corrects your input
//                            early if your current input would hit something.
//
// 1 tick = 1 physics step (~4.17ms at 240 TPS).

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Settings helpers
// ---------------------------------------------------------------------------
static bool   B(char const* k) { return Mod::get()->getSettingValue<bool>(k); }
static int    I(char const* k) { return (int)Mod::get()->getSettingValue<int64_t>(k); }
static float  F(char const* k) { return (float)Mod::get()->getSettingValue<double>(k); }


// ---------------------------------------------------------------------------
// External control: a terminal app (tickbuffer_ctl.py) writes "early late" into
// a small text file; we poll it ~8x/second. While the file exists (and the
// "external-control" setting is on) it overrides the early/late sliders.
// ---------------------------------------------------------------------------
static bool g_extActive = false;
static int  g_extEarly = 0, g_extLate = 0;

static std::filesystem::path extPath() {
#ifdef GEODE_IS_WINDOWS
    char const* base = std::getenv("LOCALAPPDATA");
    return std::filesystem::path(base ? base : ".") / "TickBuffer" / "mode.txt";
#else
    char const* base = std::getenv("HOME");
    return std::filesystem::path(base ? base : ".") / ".tickbuffer" / "mode.txt";
#endif
}

static void pollExternal() {
    std::error_code ec;
    auto path = extPath();
    if (!Mod::get()->getSettingValue<bool>("external-control") ||
        !std::filesystem::exists(path, ec)) {
        g_extActive = false;
        return;
    }
    std::ifstream f(path);
    int e = 0, l = 0;
    if (f >> e >> l) {
        g_extEarly = std::clamp(e, 0, 240);
        g_extLate  = std::clamp(l, 0, 240);
        g_extActive = true;
    }
}

static int earlyT() { return g_extActive ? g_extEarly : (int)Mod::get()->getSettingValue<int64_t>("early-ticks"); }
static int lateT()  { return g_extActive ? g_extLate  : (int)Mod::get()->getSettingValue<int64_t>("late-ticks"); }

static bool anyAssistActive() {
    return B("enabled") && (
        ((B("orb-buffer") || B("ground-buffer")) && (earlyT() > 0 || lateT() > 0))
        || B("wave-assist"));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct PlayerState {
    // input
    bool userHolding = false;       // what the human is physically doing
    bool actualHolding = false;     // what we told the player object
    int64_t lastPressTick = -100000;
    bool pressConsumed = true;

    // orbs
    Ref<RingObject> lastRing = nullptr;
    int64_t lastRingTick = -100000;

    // ground
    bool wasOnGround = false;
    bool jumpedFromGround = false;
    bool coyoteValid = false;
    int64_t coyoteTick = -100000;

    // wave
    CCPoint prevPos = {0, 0};
    float waveDx = 0.f;             // measured horizontal travel per tick
    float waveDy = 0.f;             // measured |vertical| travel per tick
};

static int64_t g_tick = 0;
static PlayerState g_state[2];
static bool g_internal = false;     // true while WE call push/release
static std::vector<std::pair<float, GameObject*>> g_hazards; // sorted by x

static void resetState() {
    g_tick = 0;
    for (auto& s : g_state) s = PlayerState();
}

static int playerIndex(PlayerObject* p) {
    auto pl = PlayLayer::get();
    if (!pl) return -1;
    if (pl->m_player1 == p) return 0;
    if (pl->m_player2 == p) return 1;
    return -1;
}

static bool isGroundMode(PlayerObject* p) {
    return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
}

static bool ringActivated(RingObject* ring, int idx) {
    return idx == 0 ? ring->m_hasBeenActivatedP1 : ring->m_hasBeenActivatedP2;
}

static void fireRing(PlayerObject* player, RingObject* ring, int idx) {
    if (idx == 0) ring->m_hasBeenActivatedP1 = true;
    else          ring->m_hasBeenActivatedP2 = true;
    player->ringJump(ring, idx == 0);
    g_state[idx].pressConsumed = true;
}

static void cacheHazards() {
    g_hazards.clear();
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_objects) return;
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj) continue;
        auto t = obj->m_objectType;
        if (t == GameObjectType::Hazard || t == GameObjectType::Solid) {
            g_hazards.emplace_back(obj->getPositionX(), obj);
        }
    }
    std::sort(g_hazards.begin(), g_hazards.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
}

static void collectNearby(float xmin, float xmax, std::vector<CCRect>& out) {
    out.clear();
    auto lo = std::lower_bound(g_hazards.begin(), g_hazards.end(), xmin - 60.f,
        [](auto const& a, float v) { return a.first < v; });
    for (auto it = lo; it != g_hazards.end() && it->first <= xmax + 60.f; ++it) {
        out.push_back(it->second->getObjectRect()); // live rect, so moving objects work
    }
}

// ---------------------------------------------------------------------------
// Wave planner: DP over (tick, net vertical steps, direction)
// ---------------------------------------------------------------------------
struct WavePlanner {
    std::vector<CCRect> rects;
    float x0 = 0, y0 = 0, dx = 0, dy = 0, half = 5;
    int K = 12;
    int sgn = 1;                        // -1 when gravity is flipped
    std::vector<int8_t> memo;           // 0 unknown, 1 survives, 2 dies
    int W = 0;

    void init() {
        W = 2 * K + 1;
        memo.assign((size_t)(K + 1) * W * 2, 0);
    }

    bool hit(float x, float y) const {
        for (auto const& r : rects) {
            if (std::abs(x - r.getMidX()) < half + r.size.width * 0.5f &&
                std::abs(y - r.getMidY()) < half + r.size.height * 0.5f) return true;
        }
        return false;
    }

    // Move one tick holding/releasing from (t, net); true if we survive to horizon.
    bool ok(int t, int net, bool hold) {
        size_t idx = ((size_t)t * W + (net + K)) * 2 + (hold ? 1 : 0);
        if (memo[idx]) return memo[idx] == 1;
        int nnet = net + (hold ? 1 : -1) * sgn;
        float x = x0 + (t + 1) * dx;
        float y = y0 + nnet * dy;
        bool res;
        if (hit(x, y)) res = false;
        else if (t + 1 >= K) res = true;
        else res = ok(t + 1, nnet, true) || ok(t + 1, nnet, false);
        memo[idx] = res ? 1 : 2;
        return res;
    }
};

static void waveAssist(PlayerObject* p, int idx) {
    auto& st = g_state[idx];
    if (!B("wave-assist") || !p->m_isDart || st.waveDx <= 0.01f || st.waveDy <= 0.01f) {
        st.actualHolding = st.userHolding;
        return;
    }

    WavePlanner pl;
    pl.K = std::clamp(I("wave-lookahead"), 1, 40);
    pl.x0 = p->getPositionX();
    pl.y0 = p->getPositionY();
    pl.dx = st.waveDx;
    pl.dy = st.waveDy;
    pl.sgn = p->m_isUpsideDown ? -1 : 1;
    pl.half = 0.5f * F("wave-hitbox") * p->m_vehicleSize + F("wave-margin");
    collectNearby(pl.x0, pl.x0 + pl.K * pl.dx, pl.rects);

    bool desired = st.userHolding;
    if (!pl.rects.empty()) {
        pl.init();
        if (!pl.ok(0, 0, desired) && pl.ok(0, 0, !desired)) {
            desired = !desired; // rescue: the other input survives the horizon
        }
    }

    if (desired != st.actualHolding) {
        g_internal = true;
        if (desired) p->pushButton(PlayerButton::Jump);
        else         p->releaseButton(PlayerButton::Jump);
        g_internal = false;
        st.actualHolding = desired;
    }
}

// ---------------------------------------------------------------------------
// PlayLayer: reset + cache + test mode
// ---------------------------------------------------------------------------
class $modify(TBPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        resetState();
        cacheHazards();
        if (anyAssistActive() && B("mark-as-test")) {
            m_isTestMode = true;
        }
    }
    void onQuit() {
        g_hazards.clear();
        resetState();
        g_extActive = false;
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// PlayerObject: ticks, input tracking, orb + ground + wave logic
// ---------------------------------------------------------------------------
class $modify(TBPlayer, PlayerObject) {
    void update(float dt) {
        int idx = playerIndex(this);
        bool active = idx >= 0 && B("enabled") && !(idx == 1 && !B("p2-too"));

        if (idx == 0 && B("enabled")) {
            ++g_tick; // one tick per physics step of P1
            if (g_tick % 30 == 1) pollExternal();
        }

        if (active) {
            g_state[idx].prevPos = this->getPosition();
            waveAssist(this, idx);
        }

        PlayerObject::update(dt);

        if (!active) return;
        auto& st = g_state[idx];

        // measure wave motion per tick
        if (m_isDart) {
            auto pos = this->getPosition();
            float ddx = pos.x - st.prevPos.x;
            float ddy = std::abs(pos.y - st.prevPos.y);
            if (ddx > 0.01f) st.waveDx = ddx;
            if (ddy > 0.01f) st.waveDy = ddy; // keep last non-zero (floor/ceiling slide)
        }

        // ground tracking
        bool onGround = m_isOnGround;
        if (B("ground-buffer") && isGroundMode(this)) {
            if (onGround && !st.wasOnGround) {
                // landed: fire a recent click if the button was released since
                st.jumpedFromGround = false;
                st.coyoteValid = false;
                if (!st.pressConsumed && !st.userHolding &&
                    (g_tick - st.lastPressTick) <= earlyT()) {
                    g_internal = true;
                    this->pushButton(PlayerButton::Jump);
                    this->releaseButton(PlayerButton::Jump);
                    g_internal = false;
                    st.pressConsumed = true;
                }
            } else if (!onGround && st.wasOnGround && !st.jumpedFromGround) {
                // walked off an edge: open the late window
                st.coyoteValid = true;
                st.coyoteTick = g_tick;
            }
        }
        st.wasOnGround = onGround;
    }

    void collidedWithObject(float dt, GameObject* obj, cocos2d::CCRect rect, bool unk) {
        PlayerObject::collidedWithObject(dt, obj, rect, unk);

        if (!B("enabled") || !B("orb-buffer")) return;
        auto ring = typeinfo_cast<RingObject*>(obj);
        if (!ring) return;
        int idx = playerIndex(this);
        if (idx < 0 || (idx == 1 && !B("p2-too"))) return;
        auto& st = g_state[idx];

        st.lastRing = ring;
        st.lastRingTick = g_tick;

        if (!st.pressConsumed && !ringActivated(ring, idx) &&
            (g_tick - st.lastPressTick) <= earlyT()) {
            fireRing(this, ring, idx);
        }
    }

    bool pushButton(PlayerButton btn) {
        int idx = playerIndex(this);
        if (g_internal || idx < 0 || btn != PlayerButton::Jump || !B("enabled") ||
            (idx == 1 && !B("p2-too"))) {
            return PlayerObject::pushButton(btn);
        }
        auto& st = g_state[idx];
        st.userHolding = true;
        st.actualHolding = true;
        st.lastPressTick = g_tick;
        st.pressConsumed = false;

        // late ground jump (coyote)
        bool restoreGround = false;
        if (B("ground-buffer") && isGroundMode(this) && !m_isOnGround && st.coyoteValid &&
            (g_tick - st.coyoteTick) <= lateT()) {
            m_isOnGround = true;
            restoreGround = true;
            st.coyoteValid = false;
            st.pressConsumed = true;
        }
        bool fromGround = m_isOnGround;

        bool res = PlayerObject::pushButton(btn);

        if (fromGround) { st.jumpedFromGround = true; st.coyoteValid = false; }
        if (restoreGround) { m_isOnGround = false; st.jumpedFromGround = true; }

        // late orb
        if (B("orb-buffer") && !st.pressConsumed) {
            auto ring = st.lastRing.data();
            if (ring && st.lastRingTick != g_tick &&
                (g_tick - st.lastRingTick) <= lateT() &&
                !ringActivated(ring, idx)) {
                fireRing(this, ring, idx);
            }
        }
        return res;
    }

    bool releaseButton(PlayerButton btn) {
        int idx = playerIndex(this);
        if (!g_internal && idx >= 0 && btn == PlayerButton::Jump) {
            g_state[idx].userHolding = false;
            g_state[idx].actualHolding = false;
        }
        return PlayerObject::releaseButton(btn);
    }
};

// ---------------------------------------------------------------------------
// Menu popup
// ---------------------------------------------------------------------------
namespace {
    struct SliderDef { char const* key; char const* label; int min; int max; };
    constexpr SliderDef SLIDERS[] = {
        {"early-ticks",    "Early", 0, 60},
        {"late-ticks",     "Late",  0, 60},
        {"wave-lookahead", "Wave lookahead", 1, 40},
    };
    struct ToggleDef { char const* key; char const* label; };
    constexpr ToggleDef TOGGLES[] = {
        {"enabled",       "Enabled"},
        {"orb-buffer",    "Orb / ring buffer"},
        {"ground-buffer", "Ground buffer"},
        {"wave-assist",   "Wave assist"},
    };
}

class BufferPopup : public Popup {
protected:
    CCLabelBMFont* m_labels[3] = {};
    CCLabelBMFont* m_summary = nullptr;

    bool init() {
        if (!Popup::init(320.f, 270.f)) return false;
        this->setTitle("Tick Buffer");
        auto size = m_mainLayer->getContentSize();
        float cx = size.width / 2;

        // Toggles (2x2 grid)
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);
        for (int i = 0; i < 4; ++i) {
            float x = (i % 2 == 0) ? cx - 75.f : cx + 55.f;
            float y = size.height - 45.f - (i / 2) * 28.f;
            auto t = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(BufferPopup::onToggle), 0.5f);
            t->setTag(i);
            t->toggle(B(TOGGLES[i].key));
            t->setPosition({x, y});
            menu->addChild(t);
            auto l = CCLabelBMFont::create(TOGGLES[i].label, "bigFont.fnt");
            l->setScale(0.3f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition({x + 14.f, y});
            m_mainLayer->addChild(l);
        }

        // Sliders
        for (int i = 0; i < 3; ++i) {
            float y = size.height - 120.f - i * 50.f;
            m_labels[i] = CCLabelBMFont::create("", "goldFont.fnt");
            m_labels[i]->setScale(0.55f);
            m_labels[i]->setPosition({cx, y + 14.f});
            m_mainLayer->addChild(m_labels[i]);

            auto s = Slider::create(this, menu_selector(BufferPopup::onSlider), 0.8f);
            s->getThumb()->setTag(i);
            s->setPosition({cx, y - 4.f});
            auto const& d = SLIDERS[i];
            s->setValue((I(d.key) - d.min) / (float)(d.max - d.min));
            m_mainLayer->addChild(s);
        }

        m_summary = CCLabelBMFont::create("", "chatFont.fnt");
        m_summary->setScale(0.55f);
        m_summary->setPosition({cx, 20.f});
        m_mainLayer->addChild(m_summary);

        refresh();
        return true;
    }

    void refresh() {
        auto ms = [](int t) { return t * 1000.f / 240.f; };
        for (int i = 0; i < 3; ++i) {
            int v = I(SLIDERS[i].key);
            m_labels[i]->setString(
                fmt::format("{}: {} ticks ({:.1f}ms)", SLIDERS[i].label, v, ms(v)).c_str());
        }
        int total = earlyT() + lateT() + 1;
        m_summary->setString(
            fmt::format("Click window: {} ticks (~{:.1f}ms @240 TPS)", total, ms(total)).c_str());
    }

    void onSlider(CCObject* sender) {
        auto thumb = static_cast<SliderThumb*>(sender);
        auto const& d = SLIDERS[thumb->getTag()];
        int v = d.min + (int)std::round(thumb->getValue() * (d.max - d.min));
        Mod::get()->setSettingValue<int64_t>(d.key, std::clamp(v, d.min, d.max));
        refresh();
    }

    void onToggle(CCObject* sender) {
        auto const& d = TOGGLES[static_cast<CCNode*>(sender)->getTag()];
        Mod::get()->setSettingValue<bool>(d.key, !B(d.key));
    }

public:
    static BufferPopup* create() {
        auto ret = new BufferPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Opening the menu: button on the pause screen
// ---------------------------------------------------------------------------
class $modify(TBPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = this->getChildByID("right-button-menu");
        if (!menu) return;
        auto spr = ButtonSprite::create("Buffer", 60, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.5f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(TBPauseLayer::onTickBuffer));
        btn->setID("tick-buffer-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }
    void onTickBuffer(CCObject*) { BufferPopup::create()->show(); }
};
