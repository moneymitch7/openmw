#include "speechcaptions.hpp"

#include <algorithm>
#include <cmath>

#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <MyGUI_RenderManager.h>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/worldmodel.hpp"

namespace MWGui
{
    namespace
    {
        // The look of Subtitles and Message Boxes Modernised (its 0.8 defaults)
        constexpr float sTextSize = 19.4f;
        constexpr float sNameSize = 15.2f;
        const MyGUI::Colour sTextColour(0.90f, 0.88f, 0.82f);
        const MyGUI::Colour sNameColour(0.79f, 0.67f, 0.44f);
        const MyGUI::Colour sShadowColour(0.f, 0.f, 0.f, 1.f);

        constexpr float sMaxAboveDistance = 1250.f; // farther: the caption goes low on the screen
        constexpr float sAboveFadeRange = 450.f;
        constexpr float sFadeIn = 0.15f;
        constexpr float sFadeOut = 0.30f;
        constexpr float sStartGrace = 0.40f; // the voice may start a frame or two after the text
        constexpr float sMinHold = 2.8f; // a line without a voice, or a very short one
        constexpr float sSecondsPerCharacter = 0.052f;
        constexpr float sLowerY = 0.81f; // where the lowest caption's text ends, as a share of the screen height
        constexpr std::size_t sMaxCaptions = 4;

        MyGUI::EditBox* makeBox(float size, const MyGUI::Colour& colour)
        {
            MyGUI::EditBox* box = MyGUI::Gui::getInstance().createWidget<MyGUI::EditBox>(
                "LuaText", MyGUI::IntCoord(0, 0, 16, 16), MyGUI::Align::Default, "HUD");
            box->setEditStatic(true);
            box->setVisibleHScroll(false);
            box->setVisibleVScroll(false);
            box->setEditMultiLine(true);
            box->setEditWordWrap(true);
            box->setTextAlign(MyGUI::Align::HCenter | MyGUI::Align::Top);
            box->setFontHeight(static_cast<int>(std::round(size)));
            box->setTextColour(colour);
            box->setTextShadow(true);
            box->setTextShadowColour(sShadowColour);
            box->setNeedMouseFocus(false);
            box->setNeedKeyFocus(false);
            box->setVisible(false);
            return box;
        }

        // Lays out a box of the given width with its text wrapped, returns its height
        int layout(MyGUI::EditBox* box, const std::string& text, int width, float size)
        {
            const int fontHeight = std::max(8, static_cast<int>(std::round(size)));
            if (box->getFontHeight() != fontHeight)
                box->setFontHeight(fontHeight);
            if (box->getWidth() != width)
                box->setSize(width, box->getHeight());
            if (box->getCaption() != text)
                box->setCaption(text);
            const int height = box->getTextSize().height;
            if (box->getHeight() != height)
                box->setSize(width, height);
            return height;
        }
    }

    SpeechCaptions::~SpeechCaptions()
    {
        clear();
    }

    void SpeechCaptions::destroy(Caption& caption)
    {
        if (caption.mNameBox)
            MyGUI::Gui::getInstance().destroyWidget(caption.mNameBox);
        if (caption.mTextBox)
            MyGUI::Gui::getInstance().destroyWidget(caption.mTextBox);
        caption.mNameBox = nullptr;
        caption.mTextBox = nullptr;
    }

    void SpeechCaptions::clear()
    {
        for (Caption& caption : mCaptions)
            destroy(caption);
        mCaptions.clear();
    }

    void SpeechCaptions::say(const MWWorld::Ptr& actor, std::string_view text)
    {
        if (actor.isEmpty() || text.find_first_not_of(" \t\r\n") == std::string_view::npos)
            return;
        MWBase::Environment::get().getWorldModel()->registerPtr(actor);
        const MWWorld::SafePtr safe(actor);

        Caption* caption = nullptr;
        for (Caption& existing : mCaptions)
            if (existing.mActor.id() == safe.id())
                caption = &existing;
        if (!caption)
        {
            if (mCaptions.size() >= sMaxCaptions)
            {
                destroy(mCaptions.front());
                mCaptions.erase(mCaptions.begin());
            }
            Caption& added = mCaptions.emplace_back();
            added.mNameBox = makeBox(sNameSize, sNameColour);
            added.mTextBox = makeBox(sTextSize, sTextColour);
            caption = &added;
        }
        caption->mActor = safe;
        caption->mIsPlayer = actor == MWMechanics::getPlayer();
        // a line said through the player is a voice in their head (Dagoth Ur's taunts): no name
        caption->mName = caption->mIsPlayer ? std::string() : std::string(actor.getClass().getName(actor));
        caption->mText = std::string(text);
        caption->mAge = 0.f;
        caption->mEnding = false;
        caption->mMinHold = std::max(sMinHold, static_cast<float>(text.size()) * sSecondsPerCharacter);
    }

    void SpeechCaptions::update(float dt, bool hidden)
    {
        if (mCaptions.empty())
            return;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWBase::SoundManager* sounds = MWBase::Environment::get().getSoundManager();
        const MyGUI::IntSize screen = MyGUI::RenderManager::getInstance().getViewSize();

        // end the lines that are over
        for (Caption& caption : mCaptions)
        {
            caption.mAge += dt;
            const MWWorld::Ptr& actor = caption.mActor.ptrOrEmpty();
            if (!caption.mEnding)
            {
                // while the voice plays, and at least the short hold (a line without a voice: by its length)
                const bool speaking = !actor.isEmpty() && sounds->sayActive(actor);
                const bool voiced = speaking || caption.mAge < sStartGrace;
                const float hold = speaking ? 0.f : caption.mMinHold;
                if (actor.isEmpty() || (!voiced && caption.mAge >= hold))
                {
                    caption.mEnding = true;
                    caption.mFadeOut = sFadeOut;
                }
            }
            else
                caption.mFadeOut -= dt;
        }
        for (auto it = mCaptions.begin(); it != mCaptions.end();)
        {
            if (it->mEnding && it->mFadeOut <= 0.f)
            {
                destroy(*it);
                it = mCaptions.erase(it);
            }
            else
                ++it;
        }

        std::vector<MyGUI::FloatPoint> placedAbove;
        float lowerBottom = sLowerY * screen.height; // the lower captions stack upwards from here
        for (auto it = mCaptions.rbegin(); it != mCaptions.rend(); ++it)
        {
            Caption& caption = *it;
            float alpha = std::min(1.f, caption.mAge / sFadeIn);
            if (caption.mEnding)
                alpha = std::min(alpha, caption.mFadeOut / sFadeOut);
            if (hidden || alpha <= 0.f)
            {
                caption.mNameBox->setVisible(false);
                caption.mTextBox->setVisible(false);
                continue;
            }

            // above the speaker's head, when it can be seen and isn't too far
            const MWWorld::Ptr& actor = caption.mActor.ptrOrEmpty();
            bool above = false;
            osg::Vec2f point;
            float distance = 0.f;
            if (!caption.mIsPlayer && !actor.isEmpty() && actor.getRefData().getBaseNode())
            {
                osg::Vec3f head = actor.getRefData().getPosition().asVec3();
                head.z() += world->getHalfExtents(actor).z() * 2.f + 18.f;
                above = world->projectToScreen(head, point, distance) && distance <= sMaxAboveDistance
                    && point.x() >= 0.f && point.x() <= 1.f && point.y() >= 0.f && point.y() <= 1.f;
            }

            if (above)
            {
                const float frameDt = std::clamp(dt, 0.001f, 0.1f);
                caption.mSmoothedDistance = caption.mSmoothedDistance < 0.f ? distance
                                                                            : caption.mSmoothedDistance
                        + (distance - caption.mSmoothedDistance) * (1.f - std::exp(-frameDt * 7.f));
                const float goal
                    = std::clamp(std::sqrt(300.f / std::max(100.f, caption.mSmoothedDistance)), 0.80f, 1.15f);
                caption.mScale += (goal - caption.mScale) * (1.f - std::exp(-frameDt * 5.f));
                const float scale = std::round(caption.mScale * 20.f) / 20.f;

                float x = std::clamp(point.x(), 0.13f, 0.87f);
                float y = std::clamp(point.y(), 0.12f, 0.91f);
                // close speakers' captions don't overlap
                for (const MyGUI::FloatPoint& other : placedAbove)
                    if (std::abs(x - other.left) < 0.30f && std::abs(y - other.top) < 0.085f)
                        y = std::max(0.12f, other.top - 0.09f);
                placedAbove.emplace_back(x, y);

                const int width = static_cast<int>(screen.width * 0.36f);
                const int height = layout(caption.mTextBox, caption.mText, width, sTextSize * scale);
                caption.mTextBox->setPosition(
                    static_cast<int>(x * screen.width - width / 2.f), static_cast<int>(y * screen.height) - height);
                caption.mTextBox->setAlpha(
                    alpha * std::clamp((sMaxAboveDistance - distance) / sAboveFadeRange, 0.f, 1.f));
                caption.mTextBox->setVisible(true);
                caption.mNameBox->setVisible(false);
            }
            else
            {
                caption.mSmoothedDistance = -1.f;
                caption.mScale = 1.f;
                const int width = static_cast<int>(screen.width * 0.64f);
                const int textHeight = layout(caption.mTextBox, caption.mText, width, sTextSize);
                const int nameHeight = layout(caption.mNameBox, caption.mName, width, sNameSize);
                const int left = static_cast<int>((screen.width - width) / 2.f);
                const int textTop = static_cast<int>(lowerBottom) - textHeight;
                caption.mTextBox->setPosition(left, textTop);
                caption.mNameBox->setPosition(left, textTop - nameHeight - 2);
                caption.mTextBox->setAlpha(alpha);
                caption.mNameBox->setAlpha(alpha);
                caption.mTextBox->setVisible(true);
                caption.mNameBox->setVisible(!caption.mName.empty());
                lowerBottom = textTop - nameHeight - 14.f;
            }
        }
    }
}
