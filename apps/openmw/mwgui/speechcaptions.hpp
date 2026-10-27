#ifndef OPENMW_MWGUI_SPEECHCAPTIONS_H
#define OPENMW_MWGUI_SPEECHCAPTIONS_H

#include <string>
#include <string_view>
#include <vector>

#include "../mwworld/ptr.hpp"

namespace MyGUI
{
    class EditBox;
}

namespace MWGui
{
    /// OpenMGE XE [GUI] subtitles above speakers: a voiced line's text shown above its speaker's head for as long as
    /// the line plays (in the style of Subtitles and Message Boxes Modernised: light text with a shadow, sized by
    /// distance), or as a caption with the speaker's name low on the screen when the speaker can't be seen (the player,
    /// off-screen, far away). Instead of the subtitle message box.
    class SpeechCaptions
    {
    public:
        SpeechCaptions() = default;
        ~SpeechCaptions();
        SpeechCaptions(const SpeechCaptions&) = delete;
        SpeechCaptions& operator=(const SpeechCaptions&) = delete;

        void say(const MWWorld::Ptr& actor, std::string_view text);

        /// @param hidden a menu is open: captions wait (out of sight) instead of being shown
        void update(float dt, bool hidden);

        void clear();

    private:
        struct Caption
        {
            MWWorld::SafePtr mActor;
            bool mIsPlayer = false;
            std::string mName;
            std::string mText;
            float mAge = 0.f;
            float mMinHold = 0.f;
            bool mEnding = false; // the line is over: fading out
            float mFadeOut = 0.f;
            float mScale = 1.f;
            float mSmoothedDistance = -1.f;
            MyGUI::EditBox* mNameBox = nullptr;
            MyGUI::EditBox* mTextBox = nullptr;
        };

        void destroy(Caption& caption);

        std::vector<Caption> mCaptions;
    };
}

#endif
