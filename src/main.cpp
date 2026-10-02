#include <Geode/Geode.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ColorPickPopup.hpp>
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
static ccColor4B loadCol(std::string const& key, ccColor4B def) {
    auto m = Mod::get();
    auto g = [&](char const* ch, int d) {
        return static_cast<GLubyte>(std::clamp(m->getSavedValue<int>(key + ch, d), 0, 255));
    };
    return ccc4(g("-r", def.r), g("-g", def.g), g("-b", def.b), g("-a", def.a));
}

static void saveCol(std::string const& key, ccColor4B c) {
    auto m = Mod::get();
    m->setSavedValue<int>(key + "-r", c.r);
    m->setSavedValue<int>(key + "-g", c.g);
    m->setSavedValue<int>(key + "-b", c.b);
    m->setSavedValue<int>(key + "-a", c.a);
}

namespace cfg {
    inline float size()        { return Mod::get()->getSavedValue<float>("size", 1.5f); }
    inline float outlineW()    { return Mod::get()->getSavedValue<float>("outline-width", 3.f); }
    inline int   shape()       { return Mod::get()->getSavedValue<int>("shape", 0); }
    inline bool  useImage()    { return Mod::get()->getSavedValue<bool>("use-image", false); }
    inline std::string image() { return Mod::get()->getSavedValue<std::string>("image-path", ""); }
    inline ccColor4B fill()    { return loadCol("fill", ccc4(255, 255, 255, 100)); }
    inline ccColor4B outline() { return loadCol("outline", ccc4(255, 255, 255, 255)); }
}

static const char* SHAPE_NAMES[] = {"Circle", "Square", "Triangle", "Pentagon", "Hexagon", "Diamond"};
static const int SHAPE_COUNT = 6;

// cocos2d's CCDrawNode uses premultiplied alpha, so colors must be multiplied by alpha
static ccColor4F pmc(ccColor4B c) {
    float a = c.a / 255.f;
    return ccc4f(c.r / 255.f * a, c.g / 255.f * a, c.b / 255.f * a, a);
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
        case 0: poly(72, 0); break;
        case 1: poly(4, PI / 4); break;
        case 2: poly(3, PI / 2); break;
        case 3: poly(5, PI / 2); break;
        case 4: poly(6, 0); break;
        case 5: poly(4, 0); break;
        default: poly(72, 0);
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
        auto fill = cfg::fill();

        if (m_img) {
            CCSize cs = m_img->getContentSize();
            float maxSide = std::max(cs.width, cs.height);
            if (maxSide < 1.f) maxSide = 1.f;
            m_img->setScale((40.f * cfg::size() * pop) / maxSide);
            m_img->setOpacity(fill.a);
            return;
        }

        float r = 18.f * cfg::size() * pop;
        auto pts = shapePoints(cfg::shape(), r);
        float ow = cfg::outlineW();
        auto out = cfg::outline();
        bool hasOutline = ow > 0.05f && out.a > 0;

        // one smooth, anti-aliased pass: fill + mitered outline
        m_draw->clear();
        m_draw->drawPolygon(
            pts.data(), static_cast<unsigned int>(pts.size()),
            pmc(fill),
            hasOutline ? ow : 0.f,
            hasOutline ? pmc(out) : ccc4f(0.f, 0.f, 0.f, 0.f)
        );
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
    ColorChannelSprite* m_fillSwatch = nullptr;
    ColorChannelSprite* m_outSwatch = nullptr;
    async::TaskHolder<Result<std::optional<std::filesystem::path>>> m_pickHolder;

    void addLabel(char const* text, float x, float y, float scale, bool centered) {
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setScale(scale);
        if (!centered) l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({x, y});
        m_mainLayer->addChild(l);
    }

    void addSlider(char const* name, int tag, float y, float norm) {
        this->addLabel(name, 16.f, y, 0.45f, false);

        auto slider = Slider::create(this, menu_selector(TapPopup::onSlider), 0.65f);
        slider->setPosition({205.f, y});
        slider->setValue(norm);
        slider->getThumb()->setTag(tag);
        m_mainLayer->addChild(slider);
    }

    void refreshSwatches() {
        auto f = cfg::fill();
        auto o = cfg::outline();
        m_fillSwatch->setColor(ccc3(f.r, f.g, f.b));
        m_outSwatch->setColor(ccc3(o.r, o.g, o.b));
    }

    bool init() {
        if (!Popup::init(320.f, 255.f)) return false;
        this->setTitle("Tap Settings");

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        // sliders
        this->addSlider("Tap Size",      0, 205.f, (cfg::size() - 0.3f) / 3.7f);
        this->addSlider("Outline Width", 1, 176.f, cfg::outlineW() / 10.f);

        // row: shape | pick image | use-image checkbox
        this->addLabel("Shape", 61.f, 157.f, 0.4f, true);
        this->addLabel("Use Image", 295.f, 157.f, 0.32f, true);

        m_shapeSpr = ButtonSprite::create(SHAPE_NAMES[cfg::shape()], 88, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.6f);
        auto shapeBtn = CCMenuItemSpriteExtra::create(m_shapeSpr, this, menu_selector(TapPopup::onShape));
        shapeBtn->setPosition({61.f, 132.f});
        menu->addChild(shapeBtn);

        auto pickSpr = ButtonSprite::create("Pick Image", 140, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.6f);
        auto pickBtn = CCMenuItemSpriteExtra::create(pickSpr, this, menu_selector(TapPopup::onPick));
        pickBtn->setPosition({197.f, 132.f});
        menu->addChild(pickBtn);

        auto toggler = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(TapPopup::onUseImage), 0.7f);
        toggler->toggle(cfg::useImage());
        toggler->setPosition({295.f, 132.f});
        menu->addChild(toggler);

        // fill color: swatch button + label
        m_fillSwatch = ColorChannelSprite::create();
        auto fillBtn = CCMenuItemSpriteExtra::create(m_fillSwatch, this, menu_selector(TapPopup::onFillColor));
        fillBtn->setPosition({31.f, 96.f});
        menu->addChild(fillBtn);
        this->addLabel("Fill Color", 56.f, 96.f, 0.45f, false);

        // outline color: swatch button + label
        m_outSwatch = ColorChannelSprite::create();
        auto outBtn = CCMenuItemSpriteExtra::create(m_outSwatch, this, menu_selector(TapPopup::onOutlineColor));
        outBtn->setPosition({31.f, 49.f});
        menu->addChild(outBtn);
        this->addLabel("Outline Color", 56.f, 49.f, 0.45f, false);

        this->refreshSwatches();
        return true;
    }

    void onSlider(CCObject* sender) {
        auto thumb = static_cast<SliderThumb*>(sender);
        float v = thumb->getValue();
        auto mod = Mod::get();
        switch (thumb->getTag()) {
            case 0: mod->setSavedValue<float>("size", 0.3f + v * 3.7f); break;
            case 1: mod->setSavedValue<float>("outline-width", v * 10.f); break;
        }
    }

    void onShape(CCObject*) {
        int next = (cfg::shape() + 1) % SHAPE_COUNT;
        Mod::get()->setSavedValue<int>("shape", next);
        m_shapeSpr->setString(SHAPE_NAMES[next]);
    }

    void openPicker(bool isFill) {
        auto current = isFill ? cfg::fill() : cfg::outline();
        auto popup = ColorPickPopup::create(current);
        if (!popup) return;
        Ref<TapPopup> self = this;
        popup->setCallback([self, isFill](ccColor4B const& c) {
            saveCol(isFill ? "fill" : "outline", c);
            self->refreshSwatches();
        });
        popup->show();
    }

    void onFillColor(CCObject*) { this->openPicker(true); }
    void onOutlineColor(CCObject*) { this->openPicker(false); }

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
