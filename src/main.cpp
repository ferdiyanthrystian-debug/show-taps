#include <Geode/Geode.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using namespace geode::prelude;

// ---------- saved settings ----------
namespace cfg {
    inline float size()        { return Mod::get()->getSavedValue<float>("size", 1.5f); }
    inline float outlineW()    { return Mod::get()->getSavedValue<float>("outline-width", 2.f); }
    inline float outlineOp()   { return Mod::get()->getSavedValue<float>("outline-opacity", 1.f); }
    inline float fillOp()      { return Mod::get()->getSavedValue<float>("fill-opacity", 0.4f); }
    inline int   shape()       { return Mod::get()->getSavedValue<int>("shape", 0); }
    inline int   fillColor()   { return Mod::get()->getSavedValue<int>("fill-color", 0); }
    inline int   outlineColor(){ return Mod::get()->getSavedValue<int>("outline-color", 0); }
    inline bool  useImage()    { return Mod::get()->getSavedValue<bool>("use-image", false); }
    inline std::string image() { return Mod::get()->getSavedValue<std::string>("image-path", ""); }
}

static const char* SHAPE_NAMES[] = {"Circle", "Square", "Triangle", "Pentagon", "Hexagon", "Diamond"};
static const int SHAPE_COUNT = 6;

struct NamedColor { const char* name; float r, g, b; };
static const NamedColor COLORS[] = {
    {"White",  1.f,  1.f,   1.f},
    {"Red",    1.f,  0.2f,  0.2f},
    {"Orange", 1.f,  0.6f,  0.1f},
    {"Yellow", 1.f,  0.95f, 0.2f},
    {"Green",  0.2f, 1.f,   0.3f},
    {"Cyan",   0.2f, 0.9f,  1.f},
    {"Blue",   0.25f,0.4f,  1.f},
    {"Purple", 0.65f,0.3f,  1.f},
    {"Pink",   1.f,  0.45f, 0.8f},
    {"Black",  0.f,  0.f,   0.f},
};
static const int COLOR_COUNT = 10;

// cocos2d's CCDrawNode uses premultiplied alpha, so colors must be multiplied by alpha
static ccColor4F pm(int idx, float a) {
    auto& c = COLORS[idx];
    return ccc4f(c.r * a, c.g * a, c.b * a, a);
}

static std::vector<CCPoint> shapePoints(int shape, float r) {
    std::vector<CCPoint> p;
    auto poly = [&](int n, float rot) {
        for (int i = 0; i < n; i++) {
            float a = rot + i * 2.f * 3.14159265f / n;
            p.push_back({cosf(a) * r, sinf(a) * r});
        }
    };
    const float PI = 3.14159265f;
    switch (shape) {
        case 0: poly(40, 0); break;
        case 1: poly(4, PI / 4); break;
        case 2: poly(3, PI / 2); break;
        case 3: poly(5, PI / 2); break;
        case 4: poly(6, 0); break;
        case 5: poly(4, 0); break;
        default: poly(40, 0);
    }
    return p;
}

// ---------- the tap effect (stays while held, follows the finger) ----------
class TapEffect : public CCNode {
protected:
    CCDrawNode* m_draw = nullptr;
    CCSprite* m_img = nullptr;
    float m_t = 0.f;
    static constexpr float POP = 0.08f;

    bool init() override {
        if (!CCNode::init()) return false;

        if (cfg::useImage() && !cfg::image().empty()) {
            m_img = CCSprite::create(cfg::image().c_str());
            if (m_img) this->addChild(m_img);
        }
        if (!m_img) {
            m_draw = CCDrawNode::create();
            this->addChild(m_draw);
        }
        this->scheduleUpdate();
        this->redraw();
        return true;
    }

    void redraw() {
        float pop = std::min(1.f, 0.6f + 0.4f * (m_t / POP));

        if (m_img) {
            CCSize cs = m_img->getContentSize();
            float maxSide = std::max(cs.width, cs.height);
            if (maxSide < 1.f) maxSide = 1.f;
            m_img->setScale((40.f * cfg::size() * pop) / maxSide);
            m_img->setOpacity(static_cast<GLubyte>(255.f * cfg::fillOp()));
            return;
        }

        float r = 18.f * cfg::size() * pop;
        auto pts = shapePoints(cfg::shape(), r);
        float ow = cfg::outlineW();
        float oa = cfg::outlineOp();

        m_draw->clear();
        // fill (no built-in border, we draw our own outline below)
        m_draw->drawPolygon(
            pts.data(), static_cast<unsigned int>(pts.size()),
            pm(cfg::fillColor(), cfg::fillOp()),
            0.f,
            ccc4f(0.f, 0.f, 0.f, 0.f)
        );
        // outline
        if (ow > 0.05f && oa > 0.f) {
            auto col = pm(cfg::outlineColor(), oa);
            size_t n = pts.size();
            for (size_t i = 0; i < n; i++) {
                m_draw->drawSegment(pts[i], pts[(i + 1) % n], ow * 0.5f, col);
            }
        }
    }

public:
    static TapEffect* create(CCPoint pos) {
        auto ret = new TapEffect();
        if (ret->init()) {
            ret->autorelease();
            ret->setPosition(pos);
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void update(float dt) override {
        m_t += dt;
        if (m_t >= POP) {
            m_t = POP;
            this->redraw();
            this->unscheduleUpdate();
            return;
        }
        this->redraw();
    }
};

static std::map<int, Ref<TapEffect>> s_active;

class $modify(TapDispatcher, CCTouchDispatcher) {
    void touches(CCSet* touches, CCEvent* event, unsigned int type) {
        CCTouchDispatcher::touches(touches, event, type);

        if (!touches) return;
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return;

        for (auto it = touches->begin(); it != touches->end(); ++it) {
            auto touch = static_cast<CCTouch*>(*it);
            int id = touch->getID();
            auto found = s_active.find(id);

            if (type == CCTOUCHBEGAN) {
                if (found != s_active.end()) {
                    if (found->second->getParent()) found->second->removeFromParent();
                    s_active.erase(found);
                }
                if (auto fx = TapEffect::create(touch->getLocation())) {
                    scene->addChild(fx, 100000);
                    s_active[id] = fx;
                }
            }
            else if (type == CCTOUCHMOVED) {
                if (found == s_active.end()) continue;
                auto fx = found->second;
                if (fx->getParent() != scene) {
                    if (fx->getParent()) fx->removeFromParent();
                    scene->addChild(fx, 100000);
                }
                fx->setPosition(touch->getLocation());
            }
            else {
                // ended or cancelled: disappear instantly
                if (found == s_active.end()) continue;
                if (found->second->getParent()) found->second->removeFromParent();
                s_active.erase(found);
            }
        }
    }
};

// ---------- settings popup ----------
class TapPopup : public Popup {
protected:
    ButtonSprite* m_shapeSpr = nullptr;
    ButtonSprite* m_fillSpr = nullptr;
    ButtonSprite* m_outSpr = nullptr;
    CCLabelBMFont* m_valueLabels[4] = {};
    async::TaskHolder<Result<std::optional<std::filesystem::path>>> m_pickHolder;

    void addSlider(char const* name, int tag, float y, float norm) {
        auto label = CCLabelBMFont::create(name, "bigFont.fnt");
        label->setScale(0.4f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({15.f, y});
        m_mainLayer->addChild(label);

        auto slider = Slider::create(this, menu_selector(TapPopup::onSlider), 0.5f);
        slider->setPosition({205.f, y});
        slider->setValue(norm);
        slider->getThumb()->setTag(tag);
        m_mainLayer->addChild(slider);

        auto val = CCLabelBMFont::create("", "bigFont.fnt");
        val->setScale(0.35f);
        val->setAnchorPoint({1.f, 0.5f});
        val->setPosition({310.f, y});
        m_mainLayer->addChild(val);
        m_valueLabels[tag] = val;
    }

    void addCaption(char const* text, float x, float y) {
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setScale(0.32f);
        l->setPosition({x, y});
        m_mainLayer->addChild(l);
    }

    void refreshLabels() {
        m_valueLabels[0]->setString(fmt::format("{:.1f}x", cfg::size()).c_str());
        m_valueLabels[1]->setString(fmt::format("{:.1f}", cfg::outlineW()).c_str());
        m_valueLabels[2]->setString(fmt::format("{}%", static_cast<int>(cfg::outlineOp() * 100)).c_str());
        m_valueLabels[3]->setString(fmt::format("{}%", static_cast<int>(cfg::fillOp() * 100)).c_str());
    }

    bool init() {
        if (!Popup::init(320.f, 285.f)) return false;
        this->setTitle("Tap Settings");

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        // sliders (normalised 0..1)
        this->addSlider("Size",            0, 240.f, (cfg::size() - 0.3f) / 3.7f);
        this->addSlider("Outline Width",   1, 212.f, cfg::outlineW() / 10.f);
        this->addSlider("Outline Opacity", 2, 184.f, cfg::outlineOp());
        this->addSlider("Fill Opacity",    3, 156.f, cfg::fillOp());
        this->refreshLabels();

        // shape / fill color / outline color buttons
        this->addCaption("Shape", 57.f, 132.f);
        this->addCaption("Fill Color", 160.f, 132.f);
        this->addCaption("Outline Color", 263.f, 132.f);

        m_shapeSpr = ButtonSprite::create(SHAPE_NAMES[cfg::shape()], 90, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.6f);
        auto shapeBtn = CCMenuItemSpriteExtra::create(m_shapeSpr, this, menu_selector(TapPopup::onShape));
        shapeBtn->setPosition({57.f, 108.f});
        menu->addChild(shapeBtn);

        m_fillSpr = ButtonSprite::create(COLORS[cfg::fillColor()].name, 90, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.6f);
        auto fillBtn = CCMenuItemSpriteExtra::create(m_fillSpr, this, menu_selector(TapPopup::onFillColor));
        fillBtn->setPosition({160.f, 108.f});
        menu->addChild(fillBtn);

        m_outSpr = ButtonSprite::create(COLORS[cfg::outlineColor()].name, 90, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.6f);
        auto outBtn = CCMenuItemSpriteExtra::create(m_outSpr, this, menu_selector(TapPopup::onOutlineColor));
        outBtn->setPosition({263.f, 108.f});
        menu->addChild(outBtn);

        // use-image toggle
        auto toggler = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(TapPopup::onUseImage), 0.7f);
        toggler->toggle(cfg::useImage());
        toggler->setPosition({30.f, 62.f});
        menu->addChild(toggler);

        auto imgLabel = CCLabelBMFont::create("Use Image", "bigFont.fnt");
        imgLabel->setScale(0.4f);
        imgLabel->setAnchorPoint({0.f, 0.5f});
        imgLabel->setPosition({52.f, 62.f});
        m_mainLayer->addChild(imgLabel);

        // pick image button
        auto pickSpr = ButtonSprite::create("Pick Image", 110, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.6f);
        auto pickBtn = CCMenuItemSpriteExtra::create(pickSpr, this, menu_selector(TapPopup::onPick));
        pickBtn->setPosition({245.f, 62.f});
        menu->addChild(pickBtn);

        return true;
    }

    void onSlider(CCObject* sender) {
        auto thumb = static_cast<SliderThumb*>(sender);
        float v = thumb->getValue();
        auto mod = Mod::get();
        switch (thumb->getTag()) {
            case 0: mod->setSavedValue<float>("size", 0.3f + v * 3.7f); break;
            case 1: mod->setSavedValue<float>("outline-width", v * 10.f); break;
            case 2: mod->setSavedValue<float>("outline-opacity", v); break;
            case 3: mod->setSavedValue<float>("fill-opacity", v); break;
        }
        this->refreshLabels();
    }

    void onShape(CCObject*) {
        int next = (cfg::shape() + 1) % SHAPE_COUNT;
        Mod::get()->setSavedValue<int>("shape", next);
        m_shapeSpr->setString(SHAPE_NAMES[next]);
    }

    void onFillColor(CCObject*) {
        int next = (cfg::fillColor() + 1) % COLOR_COUNT;
        Mod::get()->setSavedValue<int>("fill-color", next);
        m_fillSpr->setString(COLORS[next].name);
    }

    void onOutlineColor(CCObject*) {
        int next = (cfg::outlineColor() + 1) % COLOR_COUNT;
        Mod::get()->setSavedValue<int>("outline-color", next);
        m_outSpr->setString(COLORS[next].name);
    }

    void onUseImage(CCObject* sender) {
        // GD toggler quirk: isToggled() is the OLD state inside the callback
        bool now = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSavedValue<bool>("use-image", now);
    }

    void onPick(CCObject*) {
        file::FilePickOptions opts;
        opts.filters.push_back({"Images", {"*.png", "*.jpg", "*.jpeg"}});

        m_pickHolder.spawn(file::pick(file::PickMode::OpenFile, opts), [](Result<std::optional<std::filesystem::path>> res) {
            if (!res.isOk()) {
                FLAlertLayer::create("Error", "Couldn't pick that image.", "OK")->show();
                return;
            }
            auto picked = res.unwrap();
            if (!picked.has_value()) return; // user cancelled
            auto src = *picked;
            auto dst = Mod::get()->getSaveDir() / ("tap-image" + src.extension().string());

            std::error_code ec;
            std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                FLAlertLayer::create("Error", "Couldn't copy the image.", "OK")->show();
                return;
            }
            // make sure a previous image at the same path isn't cached
            CCTextureCache::get()->removeTextureForKey(dst.string().c_str());
            Mod::get()->setSavedValue<std::string>("image-path", dst.string());
            Mod::get()->setSavedValue<bool>("use-image", true);
            FLAlertLayer::create("Done", "Image set! Reopen this menu to see the toggle update.", "OK")->show();
        });
    }

public:
    static TapPopup* create() {
        auto ret = new TapPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------- pause menu button ----------
class $modify(TapPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn02_001.png");
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (!spr) return;
        spr->setScale(0.7f);

        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(TapPause::onTapSettings));

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto fallback = CCMenu::create();
            auto win = CCDirector::get()->getWinSize();
            fallback->setPosition({win.width - 30.f, win.height / 2.f});
            fallback->addChild(btn);
            this->addChild(fallback);
        }
    }

    void onTapSettings(CCObject*) {
        TapPopup::create()->show();
    }
};
