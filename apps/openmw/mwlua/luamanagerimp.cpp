#include "luamanagerimp.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>

#include <MyGUI_InputManager.h>
#include <osg/Stats>

#include <sol/object.hpp>
#include <sol/table.hpp>
#include <sol/types.hpp>

#include <components/debug/debuglog.hpp>

#include <components/esm/luascripts.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>

#include <components/settings/values.hpp>

#include <components/l10n/manager.hpp>

#include <components/lua/util.hpp>
#include <components/lua_ui/registerscriptsettings.hpp>
#include <components/lua_ui/util.hpp>

#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/aisequence.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwrender/bonegroup.hpp"
#include "../mwrender/postprocessor.hpp"
#include "../mwworld/class.hpp"

#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/worldmodel.hpp"

#include "luabindings.hpp"
#include "playerscripts.hpp"
#include "types/types.hpp"
#include "userdataserializer.hpp"

namespace MWLua
{

    static constexpr float profileAvgCoef = 1.0f / 30; // averaging over approximately 30 frames

    namespace
    {
        struct BoolScopeGuard
        {
            bool& mValue;
            BoolScopeGuard(bool& value)
                : mValue(value)
            {
                mValue = true;
            }

            ~BoolScopeGuard() { mValue = false; }
        };

        LocalScripts* asLocal(const LuaUtil::ScriptsContainerWeakPtr& ptr)
        {
            auto scripts = static_cast<LocalScripts*>(*ptr);
            if (scripts == nullptr)
                Log(Debug::Warning) << "Found local Lua script that outlived its object";
            return scripts;
        }

        // std::less rather than <: comparing pointers into unrelated objects with < is
        // unspecified, and lower_bound needs an order it can rely on.
        auto findActive(std::vector<LuaUtil::ScriptsContainerWeakPtr>& scripts, const LuaUtil::ScriptsContainer* target)
        {
            return std::lower_bound(scripts.begin(), scripts.end(), target,
                [](const LuaUtil::ScriptsContainerWeakPtr& e, const LuaUtil::ScriptsContainer* t) {
                    return std::less<const LuaUtil::ScriptsContainer*>{}(*e, t);
                });
        }

        void insertActive(std::vector<LuaUtil::ScriptsContainerWeakPtr>& scripts, LuaUtil::ScriptsContainerWeakPtr ptr)
        {
            const auto it = findActive(scripts, *ptr);
            if (it == scripts.end() || *(*it) != *ptr)
                scripts.insert(it, std::move(ptr));
        }

        // By index, not iterator: the bodies run script handlers, and a handler reaching
        // insertActive would reallocate underneath one.
        template <class F>
        void forEachActive(std::vector<LuaUtil::ScriptsContainerWeakPtr>& scripts, F&& f)
        {
            [[maybe_unused]] const std::size_t before = scripts.size();
            for (std::size_t i = 0; i < scripts.size(); ++i)
                f(asLocal(scripts[i]));
            assert(scripts.size() == before && "active local scripts changed while iterating");
        }
    }

    static LuaUtil::LuaStateSettings createLuaStateSettings()
    {
        if (!Settings::lua().mLuaProfiler)
            LuaUtil::LuaState::disableProfiler();
        return { .mInstructionLimit = Settings::lua().mInstructionLimitPerCall,
            .mMemoryLimit = Settings::lua().mMemoryLimit,
            .mSmallAllocMaxSize = Settings::lua().mSmallAllocMaxSize,
            .mLogMemoryUsage = Settings::lua().mLogMemoryUsage };
    }

    LuaManager::LuaManager(const VFS::Manager* vfs, const std::filesystem::path& libsDir)
        : mLua(vfs, &mConfiguration, createLuaStateSettings())
    {
        Log(Debug::Info) << "Lua version: " << LuaUtil::getLuaVersion();
        mLua.addInternalLibSearchPath(libsDir);

        mGlobalSerializer = createUserdataSerializer(false);
        mLocalSerializer = createUserdataSerializer(true);
        mGlobalLoader = createUserdataSerializer(false, &mContentFileMapping);
        mLocalLoader = createUserdataSerializer(true, &mContentFileMapping);

        mGlobalScripts.setSerializer(mGlobalSerializer.get());
    }

    LuaManager::~LuaManager()
    {
        LuaUi::clearSettings();
    }

    void LuaManager::initConfiguration(bool reload)
    {
        mConfiguration.init(MWBase::Environment::get().getESMStore()->getLuaScriptsCfg(), reload);
        Log(Debug::Verbose) << "Lua scripts configuration (" << mConfiguration.size() << " scripts):";
        for (size_t i = 0; i < mConfiguration.size(); ++i)
            Log(Debug::Verbose) << "#" << i << " " << LuaUtil::scriptCfgToString(mConfiguration[i]);
        mMenuScripts.setAutoStartConf(mConfiguration.getMenuConf());
        mGlobalScripts.setAutoStartConf(mConfiguration.getGlobalConf());
    }

    void LuaManager::initPreLoad()
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            Context context;
            context.mType = Context::Load;
            context.mLuaManager = this;
            context.mLua = &mLua;

            for (const auto& [name, package] : initCommonPackages(context))
                mLua.addCommonPackage(name, package);

            for (const auto& [name, package] : initLoadPackages(context))
                mLoadScripts.addPackage(name, package);

            mLoadScripts.addPackage("openmw.storage", LuaUtil::LuaStorage::initLoadPackage(view, &mPlayerStorage));

            LuaUtil::LuaStorage::initLuaBindings(view);
        });
    }

    void LuaManager::contentFilesLoaded()
    {
        initConfiguration(false);
        mLoadScripts.setAutoStartConf(mConfiguration.getLoadConf());
        mLoadScripts.addAutoStartedScripts();
        mLoadScripts.contentFilesLoaded();
        mLoadScripts.removeAllScripts();
    }

    void LuaManager::initPostLoad()
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            Context globalContext;
            globalContext.mType = Context::Global;
            globalContext.mLuaManager = this;
            globalContext.mLua = &mLua;
            globalContext.mObjectLists = &mObjectLists;
            globalContext.mLuaEvents = &mLuaEvents;
            globalContext.mSerializer = mGlobalSerializer.get();

            Context localContext = globalContext;
            localContext.mType = Context::Local;
            localContext.mSerializer = mLocalSerializer.get();

            Context menuContext = globalContext;
            menuContext.mType = Context::Menu;

            for (const auto& [name, package] : initGlobalPackages(globalContext))
                mGlobalScripts.addPackage(name, package);
            for (const auto& [name, package] : initMenuPackages(menuContext))
                mMenuScripts.addPackage(name, package);

            mLocalPackages = initLocalPackages(localContext);

            mPlayerPackages = initPlayerPackages(localContext);
            mPlayerPackages.insert(mLocalPackages.begin(), mLocalPackages.end());

            mGlobalScripts.addPackage("openmw.storage", LuaUtil::LuaStorage::initGlobalPackage(view, &mGlobalStorage));
            mMenuScripts.addPackage(
                "openmw.storage", LuaUtil::LuaStorage::initMenuPackage(view, &mGlobalStorage, &mPlayerStorage));
            mLocalPackages["openmw.storage"] = LuaUtil::LuaStorage::initLocalPackage(view, &mGlobalStorage);
            mPlayerPackages["openmw.storage"]
                = LuaUtil::LuaStorage::initPlayerPackage(view, &mGlobalStorage, &mPlayerStorage);

            mPlayerStorage.setActive(true);
            mGlobalStorage.setActive(false);

            mInitialized = true;
            mMenuScripts.addAutoStartedScripts();
        });
    }

    void LuaManager::loadPermanentStorage(const std::filesystem::path& userConfigPath)
    {
        mUserConfigPath = userConfigPath;
        mPlayerStorage.setActive(true);
        mGlobalStorage.setActive(true);
        const auto globalPath = userConfigPath / "global_storage.bin";
        const auto playerPath = userConfigPath / "player_storage.bin";

        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            if (std::filesystem::exists(globalPath))
                mGlobalStorage.load(view.sol(), globalPath);
            if (std::filesystem::exists(playerPath))
                mPlayerStorage.load(view.sol(), playerPath);
        });
    }

    void LuaManager::savePermanentStorage(const std::filesystem::path& userConfigPath)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            if (mGlobalScriptsStarted)
                mGlobalStorage.save(view.sol(), userConfigPath / "global_storage.bin");
            mPlayerStorage.save(view.sol(), userConfigPath / "player_storage.bin");
        });
    }

    void LuaManager::sendLocalEvent(
        const MWWorld::Ptr& target, const std::string& name, const std::optional<sol::table>& data)
    {
        LuaUtil::BinaryData binary = {};
        if (data)
        {
            binary = LuaUtil::serialize(*data, mLocalSerializer.get());
        }
        mLuaEvents.addLocalEvent({ getId(target), name, std::move(binary) });
    }

    void LuaManager::update()
    {
        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        MWWorld::Ptr newPlayerPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (!(getId(mPlayer) == getId(newPlayerPtr)))
            throw std::logic_error("Player RefNum was changed unexpectedly");
        if (!mPlayer.isInCell() || !newPlayerPtr.isInCell() || mPlayer.getCell() != newPlayerPtr.getCell())
        {
            mPlayer = newPlayerPtr; // player was moved to another cell, update ptr in registry
            MWBase::Environment::get().getWorldModel()->registerPtr(mPlayer);
        }

        const bool profiling = LuaUtil::LuaState::isProfilerEnabled();
        auto phaseStart = std::chrono::steady_clock::now();
        auto endPhase = [&](ProfilePhase phase) {
            if (!profiling)
                return;
            addPhaseTime(phase, phaseStart);
            phaseStart = std::chrono::steady_clock::now();
        };

        mObjectLists.update();

        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mQueuedAutoStartedScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->addAutoStartedScripts();
        }
        mQueuedAutoStartedScripts.clear();

        std::erase_if(mActiveLocalScripts, [](const LuaUtil::ScriptsContainerWeakPtr& ptr) {
            LocalScripts* l = asLocal(ptr);
            return l == nullptr || l->getPtrOrEmpty().isEmpty() || l->getPtrOrEmpty().mRef->isDeleted();
        });

        mGlobalScripts.statsNextFrame();
        mMenuScripts.statsNextFrame(); // its averages decay per frame too
        forEachActive(mActiveLocalScripts, [](LocalScripts* scripts) { scripts->statsNextFrame(); });

        mLuaEvents.finalizeEventBatch();

        MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();
        const double realTime = LuaUtil::getRealTime();

        double simulationTime = timeManager.isPaused() ? 0 : timeManager.getSimulationTime();
        double gameTime = timeManager.isPaused() ? 0 : timeManager.getGameTime();

        endPhase(Phase_ObjectLists);

        // Always process real-time timers (runs even when paused), but only process game/simulation timers when not
        // paused
        mMenuScripts.processTimers(simulationTime, gameTime, realTime);
        mGlobalScripts.processTimers(simulationTime, gameTime, realTime);
        forEachActive(mActiveLocalScripts,
            [&](LocalScripts* scripts) { scripts->processTimers(simulationTime, gameTime, realTime); });

        endPhase(Phase_Timers);

        // Run event handlers for events that were sent before `finalizeEventBatch`.
        mLuaEvents.callEventHandlers();
        endPhase(Phase_Events);

        mLua.protectedCall([&](LuaUtil::LuaView& lua) {
            // Run queued callbacks
            for (CallbackWithData& c : mQueuedCallbacks)
                c.mCallback.tryCall(c.mArg);
            mQueuedCallbacks.clear();

            // Run engine handlers
            mEngineEvents.callEngineHandlers();
            bool isPaused = timeManager.isPaused();
            endPhase(Phase_EngineHandlers);

            float frameDuration = MWBase::Environment::get().getFrameDuration();
            const unsigned distantInterval
                = isPaused ? 1u : static_cast<unsigned>(Settings::lua().mDistantUpdateInterval.get());
            if (distantInterval <= 1)
                forEachActive(
                    mActiveLocalScripts, [&](LocalScripts* scripts) { scripts->update(isPaused ? 0 : frameDuration); });
            else
            {
                // Objects far from the player run onUpdate every few frames (with the skipped time added up),
                // spread over the frames; near ones, actors in combat and the player every frame.
                ++mDistantUpdateFrame;
                const osg::Vec3f playerPos = mPlayer.getRefData().getPosition().asVec3();
                const float distance = Settings::lua().mDistantUpdateDistance;
                const float maxDistanceSq = distance * distance;
                forEachActive(mActiveLocalScripts, [&](LocalScripts* scripts) {
                    const MWWorld::Ptr& ptr = scripts->getPtrOrEmpty();
                    bool fullRate = ptr.isEmpty() || !ptr.isInCell() || ptr == mPlayer
                        || (ptr.getRefData().getPosition().asVec3() - playerPos).length2() <= maxDistanceSq;
                    if (!fullRate && ptr.getClass().isActor())
                        fullRate = ptr.getClass().getCreatureStats(ptr).getAiSequence().isInCombat();
                    if (const std::optional<float> dt = scripts->updateThrottle().next(
                            frameDuration, fullRate, mDistantUpdateFrame, scripts->updatePhase(), distantInterval))
                        scripts->update(*dt);
                });
            }
            endPhase(Phase_LocalUpdate);
            mGlobalScripts.update(isPaused ? 0 : frameDuration);
            endPhase(Phase_GlobalUpdate);

            mScriptTracker.unloadInactiveScripts(lua);
        });
    }

    bool LuaManager::gcStep(int steps)
    {
        // OpenMGE XE: a step that runs long (LuaJIT's atomic phase cannot be split, and grows with the heap) holds
        // up the next frame's Lua update; name it so such hitches are not blamed on scripts.
        const auto start = std::chrono::steady_clock::now();
        const bool finished = lua_gc(mLua.unsafeState(), LUA_GCSTEP, steps) == 1;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms >= 20.0)
            Log(Debug::Info) << "Slow Lua garbage collection step: " << static_cast<int>(ms) << " ms (Lua memory "
                             << lua_gc(mLua.unsafeState(), LUA_GCCOUNT, 0) / 1024 << " MB)";
        return finished;
    }

    void LuaManager::objectTeleported(const MWWorld::Ptr& ptr)
    {
        if (ptr == mPlayer)
        {
            // For player run the onTeleported handler immediately,
            // so it can adjust camera position after teleporting.
            PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
            if (playerScripts)
                playerScripts->onTeleported();
        }
        else
            mEngineEvents.addToQueue(EngineEvents::OnTeleported{ getId(ptr) });
    }

    void LuaManager::questUpdated(const ESM::RefId& questId, int stage)
    {
        if (mPlayer.isEmpty())
            return; // The game is not started yet.
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
        {
            playerScripts->onQuestUpdate(questId.serializeText(), stage);
        }
    }

    void LuaManager::synchronizedUpdate()
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) { synchronizedUpdateUnsafe(); });
    }

    void LuaManager::synchronizedUpdateUnsafe()
    {
        if (mNewGameStarted)
        {
            mNewGameStarted = false;
            // Run onNewGame handler in synchronizedUpdate (at the beginning of the frame), so it
            // can teleport the player to the starting location before the first frame is rendered.
            mGlobalScripts.newGameStarted();
        }
        BoolScopeGuard updateGuard(mRunningSynchronizedUpdates);

        if (LuaUtil::LuaState::isProfilerEnabled() && !mUserConfigPath.empty())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= mNextProfileReport)
            {
                mNextProfileReport = now + std::chrono::seconds(5);
                writeProfileReport();
            }
        }

        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        PlayerScripts* playerScripts
            = mPlayer.isEmpty() ? nullptr : dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        const bool profiling = LuaUtil::LuaState::isProfilerEnabled();
        const auto syncStart = std::chrono::steady_clock::now();
        // We apply input events in `synchronizedUpdate` rather than in `update` in order to reduce input latency.
        {
            BoolScopeGuard processingGuard(mProcessingInputEvents);

            for (const auto& event : mMenuInputEvents)
                mMenuScripts.processInputEvent(event);
            mMenuInputEvents.clear();
            if (playerScripts && !windowManager->containsMode(MWGui::GM_MainMenu))
            {
                for (const auto& event : mInputEvents)
                    playerScripts->processInputEvent(event);
            }
            mInputEvents.clear();
            mLuaEvents.callMenuEventHandlers();
            float frameDuration = MWBase::Environment::get().getWorld()->getTimeManager()->isPaused()
                ? 0.f
                : MWBase::Environment::get().getFrameDuration();
            mInputActions.update(frameDuration);
            mMenuScripts.onFrame(frameDuration);
            if (playerScripts)
                playerScripts->onFrame(frameDuration);
        }

        if (profiling)
            addPhaseTime(Phase_SyncInputAndFrame, syncStart);

        for (const auto& [message, mode] : mUIMessages)
            windowManager->messageBox(message, mode);
        mUIMessages.clear();
        for (auto& [msg, color] : mInGameConsoleMessages)
            windowManager->printToConsole(msg, "#" + color.toHex());
        mInGameConsoleMessages.clear();

        applyDelayedActions();

        if (mReloadAllScriptsRequested)
        {
            // Reloading right after `applyDelayedActions` to guarantee that no delayed actions are currently queued.
            reloadAllScriptsImpl();
            mReloadAllScriptsRequested = false;
        }

        if (mDelayedUiModeChangedArg)
        {
            if (playerScripts)
                playerScripts->uiModeChanged(*mDelayedUiModeChangedArg, true);
            mDelayedUiModeChangedArg = std::nullopt;
        }
    }

    void LuaManager::applyDelayedActions()
    {
        BoolScopeGuard applyingGuard(mApplyingDelayedActions);
        if (!LuaUtil::LuaState::isProfilerEnabled())
        {
            for (DelayedAction& action : mActionQueue)
                action.apply();
        }
        else
        {
            // Profiler: time per kind of queued change
            const auto start = std::chrono::steady_clock::now();
            auto actionStart = start;
            for (DelayedAction& action : mActionQueue)
            {
                action.apply();
                const auto end = std::chrono::steady_clock::now();
                mQueuedChangeKey = action.name().empty() ? "(unnamed)" : action.name();
                if (!action.script().empty())
                {
                    mQueuedChangeKey += "  from ";
                    mQueuedChangeKey += action.script();
                }
                auto it = mQueuedChangeStats.find(mQueuedChangeKey);
                if (it == mQueuedChangeStats.end())
                    it = mQueuedChangeStats.emplace(mQueuedChangeKey, QueuedChangeStats{}).first;
                it->second.mFrameCount += 1;
                it->second.mFrameMs += std::chrono::duration<double, std::milli>(end - actionStart).count();
                actionStart = end;
            }
            for (auto& [name, stats] : mQueuedChangeStats)
            {
                stats.mAvgCount += (stats.mFrameCount - stats.mAvgCount) * profileAvgCoef;
                stats.mAvgMs += (static_cast<float>(stats.mFrameMs) - stats.mAvgMs) * profileAvgCoef;
                stats.mFrameCount = 0;
                stats.mFrameMs = 0;
            }
            addPhaseTime(Phase_SyncQueuedChanges, start);
        }
        mActionQueue.clear();

        if (mTeleportPlayerAction)
            mTeleportPlayerAction->apply();
        mTeleportPlayerAction.reset();
    }

    void LuaManager::clear()
    {
        mActionQueue.clear();
        mTeleportPlayerAction.reset();
        LuaUi::clearGameInterface();
        for (const std::string& name : mUiResourceManager.gameCursorNames())
            MWBase::Environment::get().getWindowManager()->removeLuaCursor(name);
        mUiResourceManager.clearGameResources();
        MWBase::Environment::get().getWorld()->getPostProcessor()->disableDynamicShaders();
        mActiveLocalScripts.clear();
        mLuaEvents.clear();
        mEngineEvents.clear();
        mInputEvents.clear();
        mMenuInputEvents.clear();
        mObjectLists.clear();
        mGlobalScripts.removeAllScripts();
        mGlobalScriptsStarted = false;
        mNewGameStarted = false;
        mDelayedUiModeChangedArg = std::nullopt;
        if (!mPlayer.isEmpty())
        {
            mPlayer.getCellRef().unsetRefNum();
            mPlayer.getRefData().setLuaScripts(nullptr);
            mPlayer = MWWorld::Ptr();
        }
        mGlobalStorage.setActive(true);
        mGlobalStorage.clearTemporaryAndRemoveCallbacks();
        mGlobalStorage.setActive(false);
        mPlayerStorage.clearTemporaryAndRemoveCallbacks();
        mInputActions.clear();
        mInputTriggers.clear();
        mQueuedAutoStartedScripts.clear();
        clearObjectCaches(mLua.unsafeState());
        for (int i = 0; i < 5; ++i)
            lua_gc(mLua.unsafeState(), LUA_GCCOLLECT, 0);
    }

    void LuaManager::setupPlayer(const MWWorld::Ptr& ptr)
    {
        if (!mInitialized)
            return;
        if (!mPlayer.isEmpty())
            throw std::logic_error("Player is initialized twice");
        mObjectLists.objectAddedToScene(ptr);
        mObjectLists.setPlayer(ptr);
        mPlayer = ptr;
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            localScripts = createLocalScripts(ptr);
            mQueuedAutoStartedScripts.push_back(localScripts->getWeakPointer());
        }
        insertActive(mActiveLocalScripts, localScripts->getWeakPointer());
        mEngineEvents.addToQueue(EngineEvents::OnActive{ getId(ptr) });
    }

    void LuaManager::newGameStarted()
    {
        mGlobalStorage.setActive(true);
        mInputEvents.clear();
        mGlobalScripts.addAutoStartedScripts();
        mGlobalScriptsStarted = true;
        mNewGameStarted = true;
    }

    void LuaManager::gameLoaded()
    {
        mGlobalStorage.setActive(true);
        if (!mGlobalScriptsStarted)
            mGlobalScripts.addAutoStartedScripts();
        mGlobalScriptsStarted = true;
        mMenuScripts.stateChanged();
    }

    void LuaManager::gameEnded()
    {
        // TODO: disable scripts and global storage when the game is actually unloaded
        // mGlobalStorage.setActive(false);
        mMenuScripts.stateChanged();
    }

    void LuaManager::noGame()
    {
        clear();
        mMenuScripts.stateChanged();
    }

    void LuaManager::uiModeChanged(const MWWorld::Ptr& arg)
    {
        if (mPlayer.isEmpty())
            return;
        ObjectId argId = arg.isEmpty() ? ObjectId() : getId(arg);
        if (mApplyingDelayedActions)
        {
            mDelayedUiModeChangedArg = argId;
            return;
        }
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
            playerScripts->uiModeChanged(argId, false);
    }

    void LuaManager::viewportResized(int width, int height)
    {
        if (!mPlayer.isEmpty())
        {
            PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
            if (playerScripts)
                playerScripts->onViewportResized(width, height);
        }

        mMenuScripts.onViewportResized(width, height);
    }

    void LuaManager::actorDied(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty())
            return;
        mLuaEvents.addLocalEvent({ getId(actor), "Died", {} });
    }

    void LuaManager::onDialogueResponse(
        const MWWorld::Ptr& actor, const ESM::DialInfo& info, const ESM::Dialogue& record)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["actor"] = LObject(actor);
            if (record.mType == ESM::Dialogue::Type::Greeting)
                data["type"] = "greeting";
            else if (record.mType == ESM::Dialogue::Type::Journal)
                data["type"] = "journal";
            else if (record.mType == ESM::Dialogue::Type::Persuasion)
                data["type"] = "persuasion";
            else if (record.mType == ESM::Dialogue::Type::Topic)
                data["type"] = "topic";
            else if (record.mType == ESM::Dialogue::Type::Voice)
                data["type"] = "voice";
            data["infoId"] = info.mId.serializeText();
            data["recordId"] = record.mId.serializeText();
            sendLocalEvent(mPlayer, "DialogueResponse", data);
        });
    }

    void LuaManager::applyMagicEffects(ESM::RefId id, const MWWorld::Ptr& caster, ESM::RefNum item,
        const MWWorld::Ptr& target, const std::vector<int>& effects, bool ignoreReflect, bool ignoreSpellAbsorption,
        bool stackable, bool isReflect)
    {
        if (!target.isEmpty() && !effects.empty())
        {
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                sol::table luaEffects = view.newTable();
                for (int i = 1; i <= static_cast<int>(effects.size()); i++)
                    luaEffects[i] = effects[i - 1];
                sol::table data = view.newTable();
                if (!caster.isEmpty())
                    data["caster"] = LObject(caster);
                if (!item.isZeroOrUnset())
                    data["item"] = LObject(item);
                data["id"] = id.serializeText();
                data["target"] = LObject(target);
                data["effects"] = luaEffects;
                data["ignoreReflect"] = ignoreReflect;
                data["ignoreSpellAbsorption"] = ignoreSpellAbsorption;
                data["stackable"] = stackable;
                data["isReflect"] = isReflect;
                sendLocalEvent(target, "ApplyMagicEffects", data);
            });
        }
    }

    void LuaManager::magicProjectileHit(ESM::RefId spellId, const MWWorld::Ptr& caster, ESM::RefNum item,
        const MWWorld::Ptr& victim, const osg::Vec3f& position, const osg::Vec3f& normal)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table projectile = view.newTable();
            sol::table hitResult = view.newTable();
            sol::table spellcast = view.newTable();
            hitResult["hit"] = true;
            hitResult["hitPos"] = position;
            hitResult["hitNormal"] = normal;
            if (!victim.isEmpty())
                hitResult["hitObject"] = GObject(victim);
            spellcast["id"] = spellId.serializeText();
            if (!caster.isEmpty())
                spellcast["caster"] = GObject(caster);
            if (item.isSet())
                spellcast["item"] = GObject(item);
            projectile["type"] = "Magic";
            projectile["userData"] = spellcast;

            mGlobalScripts.onProjectileHit(projectile, hitResult);
        });
    }

    void LuaManager::useItem(const MWWorld::Ptr& object, const MWWorld::Ptr& actor, bool force)
    {
        MWBase::Environment::get().getWorldModel()->registerPtr(object);
        mEngineEvents.addToQueue(EngineEvents::OnUseItem{ getId(actor), getId(object), force });
    }

    void LuaManager::objectDropped(
        const MWWorld::Ptr& object, const MWWorld::Ptr& actor, const osg::Vec3f& position, const osg::Quat& rotation)
    {
        MWBase::Environment::get().getWorldModel()->registerPtr(object);
        mEngineEvents.addToQueue(
            EngineEvents::OnDropped{ getId(object), getId(actor), position, LuaUtil::asTransform(rotation) });
    }

    void LuaManager::objectPlaced(
        const MWWorld::Ptr& object, const MWWorld::Ptr& actor, const osg::Vec3f& position, const osg::Quat& rotation)
    {
        MWBase::Environment::get().getWorldModel()->registerPtr(object);
        mEngineEvents.addToQueue(
            EngineEvents::OnPlaced{ getId(object), getId(actor), position, LuaUtil::asTransform(rotation) });
    }

    void LuaManager::animationTextKey(const MWWorld::Ptr& actor, const std::string& key)
    {
        auto pos = key.find(": ");
        if (pos != std::string::npos)
            mEngineEvents.addToQueue(
                EngineEvents::OnAnimationTextKey{ getId(actor), key.substr(0, pos), key.substr(pos + 2) });
    }

    void LuaManager::playAnimation(const MWWorld::Ptr& actor, const std::string& groupname,
        const MWRender::AnimPriority& priority, int blendMask, bool autodisable, float speedmult,
        std::string_view start, std::string_view stop, float startpoint, uint32_t loops, bool loopfallback)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table options = view.newTable();
            options["blendMask"] = blendMask;
            options["autoDisable"] = autodisable;
            options["speed"] = speedmult;
            options["startKey"] = start;
            options["stopKey"] = stop;
            options["startPoint"] = startpoint;
            options["loops"] = loops;
            options["forceLoop"] = loopfallback;

            bool priorityAsTable = false;
            for (uint32_t i = 1; i < MWRender::sNumBlendMasks; i++)
                if (priority[static_cast<MWRender::BoneGroup>(i)] != priority[static_cast<MWRender::BoneGroup>(0)])
                    priorityAsTable = true;
            if (priorityAsTable)
            {
                sol::table priorityTable = view.newTable();
                for (uint32_t i = 0; i < MWRender::sNumBlendMasks; i++)
                    priorityTable[static_cast<MWRender::BoneGroup>(i)] = priority[static_cast<MWRender::BoneGroup>(i)];
                options["priority"] = priorityTable;
            }
            else
                options["priority"] = priority[MWRender::BoneGroup_LowerBody];

            // mEngineEvents.addToQueue(event);
            //  Has to be called immediately, otherwise engine details that depend on animations playing immediately
            //  break.
            if (auto* scripts = actor.getRefData().getLuaScripts())
                scripts->onPlayAnimation(groupname, options);
        });
    }

    void LuaManager::animationEnded(const MWWorld::Ptr& actor, std::string_view groupname, float time, float completion,
        std::string_view startKey, std::string_view stopKey)
    {
        mEngineEvents.addToQueue(EngineEvents::OnAnimationEnded{
            getId(actor), std::string(groupname), std::string(startKey), std::string(stopKey), time, completion });
    }

    void LuaManager::skillUse(const MWWorld::Ptr& actor, ESM::RefId skillId, int useType, float scale)
    {
        mEngineEvents.addToQueue(EngineEvents::OnSkillUse{ getId(actor), skillId.serializeText(), useType, scale });
    }

    void LuaManager::skillLevelUp(const MWWorld::Ptr& actor, ESM::RefId skillId, std::string_view source)
    {
        mEngineEvents.addToQueue(
            EngineEvents::OnSkillLevelUp{ getId(actor), skillId.serializeText(), std::string(source) });
    }

    void LuaManager::jailTimeServed(const MWWorld::Ptr& actor, int days)
    {
        mEngineEvents.addToQueue(EngineEvents::OnJailTimeServed{ getId(actor), days });
    }

    void LuaManager::onHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, const MWWorld::Ptr& weapon,
        const MWWorld::Ptr& ammo, int attackType, float attackStrength, float attackWindUp, float damage, bool isHealth,
        const osg::Vec3f& hitPos, bool successful, MWMechanics::DamageSourceType sourceType)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table damageTable = view.newTable();
            if (isHealth)
                damageTable["health"] = damage;
            else
                damageTable["fatigue"] = damage;

            sol::table data = view.newTable();
            if (!attacker.isEmpty())
                data["attacker"] = LObject(attacker);
            if (!weapon.isEmpty())
                data["weapon"] = LObject(weapon);
            if (!ammo.isEmpty())
                data["ammo"] = ammo.getCellRef().getRefId().serializeText();
            data["type"] = attackType;
            data["strength"] = attackStrength;
            data["windUp"] = attackWindUp;
            data["damage"] = damageTable;
            data["hitPos"] = hitPos;
            data["successful"] = successful;
            switch (sourceType)
            {
                case MWMechanics::DamageSourceType::Unspecified:
                    data["sourceType"] = "unspecified";
                    break;
                case MWMechanics::DamageSourceType::Melee:
                    data["sourceType"] = "melee";
                    break;
                case MWMechanics::DamageSourceType::Ranged:
                    data["sourceType"] = "ranged";
                    break;
                case MWMechanics::DamageSourceType::Magical:
                    data["sourceType"] = "magic";
                    break;
            }

            sendLocalEvent(victim, "Hit", data);
        });
    }

    void LuaManager::objectAddedToScene(const MWWorld::Ptr& ptr)
    {
        mObjectLists.objectAddedToScene(ptr); // assigns generated RefNum if it is not set yet.
        mEngineEvents.addToQueue(EngineEvents::OnActive{ getId(ptr) });

        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            LuaUtil::ScriptIdsWithInitializationData autoStartConf
                = mConfiguration.getLocalConf(getLiveCellRefType(ptr.mRef), ptr.getCellRef().getRefId(), getId(ptr));
            if (!autoStartConf.empty())
            {
                localScripts = createLocalScripts(ptr, std::move(autoStartConf));
                mQueuedAutoStartedScripts.push_back(localScripts->getWeakPointer());
            }
        }
        if (localScripts)
            insertActive(mActiveLocalScripts, localScripts->getWeakPointer());
    }

    void LuaManager::objectRemovedFromScene(const MWWorld::Ptr& ptr)
    {
        mObjectLists.objectRemovedFromScene(ptr);
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (localScripts)
        {
            const auto it = findActive(mActiveLocalScripts, localScripts);
            if (it != mActiveLocalScripts.end() && *(*it) == localScripts)
                mActiveLocalScripts.erase(it);
            if (!MWBase::Environment::get().getWorldModel()->getPtr(getId(ptr)).isEmpty())
                mEngineEvents.addToQueue(EngineEvents::OnInactive{ getId(ptr) });
        }
    }

    void LuaManager::inputEvent(const InputEvent& event)
    {
        if (!MyGUI::InputManager::getInstance().isModalAny()
            && !MWBase::Environment::get().getWindowManager()->isConsoleMode())
        {
            mInputEvents.push_back(event);
        }
        mMenuInputEvents.push_back(event);
    }

    MWBase::LuaManager::ActorControls* LuaManager::getActorControls(const MWWorld::Ptr& ptr) const
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
            return nullptr;
        return localScripts->getActorControls();
    }

    void LuaManager::addCustomLocalScript(const MWWorld::Ptr& ptr, int scriptId, std::string_view initData)
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            localScripts = createLocalScripts(ptr);
            localScripts->addAutoStartedScripts();
            if (ptr.isInCell() && MWBase::Environment::get().getWorldScene()->isCellActive(*ptr.getCell()))
            {
                localScripts->setActive(true, false);
                insertActive(mActiveLocalScripts, localScripts->getWeakPointer());
            }
        }
        localScripts->addCustomScript(scriptId, initData);
    }

    LocalScripts* LuaManager::createLocalScripts(
        const MWWorld::Ptr& ptr, std::optional<LuaUtil::ScriptIdsWithInitializationData> autoStartConf)
    {
        assert(mInitialized);
        std::shared_ptr<LocalScripts> scripts;
        const uint32_t type = getLiveCellRefType(ptr.mRef);
        if (type == ESM::REC_STAT)
            throw std::runtime_error("Lua scripts on static objects are not allowed");
        else if (type == ESM::REC_INTERNAL_PLAYER)
        {
            scripts = std::make_shared<PlayerScripts>(&mLua, LObject(getId(ptr)));
            scripts->setAutoStartConf(mConfiguration.getPlayerConf());
            for (const auto& [name, package] : mPlayerPackages)
                scripts->addPackage(name, package);
        }
        else
        {
            scripts = std::make_shared<LocalScripts>(&mLua, LObject(getId(ptr)), &mScriptTracker);
            if (!autoStartConf.has_value())
                autoStartConf = mConfiguration.getLocalConf(type, ptr.getCellRef().getRefId(), getId(ptr));
            scripts->setAutoStartConf(std::move(*autoStartConf));
            for (const auto& [name, package] : mLocalPackages)
                scripts->addPackage(name, package);
        }
        scripts->setSerializer(mLocalSerializer.get());

        MWWorld::RefData& refData = ptr.getRefData();
        refData.setLuaScripts(std::move(scripts));
        return refData.getLuaScripts();
    }

    void LuaManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        writer.startRecord(ESM::REC_LUAM);

        writer.writeHNT<double>("LUAW", MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime());
        writer.writeFormId(MWBase::Environment::get().getWorldModel()->getLastGeneratedRefNum(), true);
        mConfiguration.write(writer);
        ESM::LuaScripts globalScripts;
        mGlobalScripts.save(globalScripts);
        globalScripts.save(writer);
        mLuaEvents.save(writer);

        writer.endRecord(ESM::REC_LUAM);
    }

    void LuaManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type != ESM::REC_LUAM)
            throw std::runtime_error("ESM::REC_LUAM is expected");

        double simulationTime;
        reader.getHNT(simulationTime, "LUAW");
        MWBase::Environment::get().getWorld()->getTimeManager()->setSimulationTime(simulationTime);
        ESM::FormId lastGenerated = reader.getFormId(true);
        if (lastGenerated.hasContentFile())
            throw std::runtime_error("Last generated RefNum is invalid");
        MWBase::Environment::get().getWorldModel()->setLastGeneratedRefNum(lastGenerated);

        mConfiguration.read(reader);

        // TODO: don't execute scripts right away, it will be necessary in multiplayer where global storage requires
        // initialization. For now just set global storage as active slightly before it would be set by gameLoaded()
        mGlobalStorage.setActive(true);

        ESM::LuaScripts globalScripts;
        globalScripts.load(reader);
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            mLuaEvents.load(view.sol(), reader, mContentFileMapping, mGlobalLoader.get());
        });

        mGlobalScripts.setSavedDataDeserializer(mGlobalLoader.get());
        mGlobalScripts.load(globalScripts);
        mGlobalScriptsStarted = true;
    }

    void LuaManager::saveLocalScripts(const MWWorld::Ptr& ptr, ESM::LuaScripts& data)
    {
        if (ptr.getRefData().getLuaScripts())
            ptr.getRefData().getLuaScripts()->save(data);
        else
            data.mScripts.clear();
    }

    void LuaManager::loadLocalScripts(const MWWorld::Ptr& ptr, const ESM::LuaScripts& data)
    {
        if (data.mScripts.empty())
        {
            if (ptr.getRefData().getLuaScripts())
                ptr.getRefData().setLuaScripts(nullptr);
            return;
        }

        MWBase::Environment::get().getWorldModel()->registerPtr(ptr);
        LocalScripts* scripts = createLocalScripts(ptr);

        scripts->setSerializer(mLocalSerializer.get());
        scripts->setSavedDataDeserializer(mLocalLoader.get());
        scripts->load(data);
    }

    void LuaManager::reloadAllScriptsImpl()
    {
        Log(Debug::Info) << "Reload Lua";

        LuaUi::clearGameInterface();
        LuaUi::clearMenuInterface();
        LuaUi::clearSettings();
        MWBase::Environment::get().getWindowManager()->setConsoleMode("");
        MWBase::Environment::get().getL10nManager()->dropCache();
        for (const std::string& name : mUiResourceManager.cursorNames())
            MWBase::Environment::get().getWindowManager()->removeLuaCursor(name);
        mUiResourceManager.clear();
        mLua.dropScriptCache();
        mInputActions.clear(true);
        mInputTriggers.clear(true);

        ESM::LuaScripts globalData;

        if (mGlobalScriptsStarted)
        {
            mGlobalScripts.setSavedDataDeserializer(mGlobalSerializer.get());
            mGlobalScripts.save(globalData);
            mGlobalStorage.clearTemporaryAndRemoveCallbacks();
        }

        std::unordered_map<ESM::RefNum, ESM::LuaScripts> localData;

        for (const auto& [id, ptr] : MWBase::Environment::get().getWorldModel()->getPtrRegistryView())
        {
            LocalScripts* scripts = ptr.getRefData().getLuaScripts();
            if (scripts == nullptr)
                continue;
            scripts->setSavedDataDeserializer(mLocalSerializer.get());
            ESM::LuaScripts data;
            scripts->save(data);
            localData[id] = std::move(data);
        }

        initConfiguration(true);

        mMenuScripts.removeAllScripts();

        mPlayerStorage.clearTemporaryAndRemoveCallbacks();

        mMenuScripts.addAutoStartedScripts();

        for (const auto& [id, ptr] : MWBase::Environment::get().getWorldModel()->getPtrRegistryView())
        {
            LocalScripts* scripts = ptr.getRefData().getLuaScripts();
            if (scripts == nullptr)
                continue;
            scripts->load(localData[id]);
        }

        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->setActive(true);
        }

        if (mGlobalScriptsStarted)
        {
            mGlobalScripts.load(globalData);
        }
    }

    void LuaManager::handleConsoleCommand(
        const std::string& consoleMode, const std::string& command, const MWWorld::Ptr& selectedPtr)
    {
        PlayerScripts* playerScripts = nullptr;
        if (!mPlayer.isEmpty())
            playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        bool processed = mMenuScripts.consoleCommand(consoleMode, command);
        if (playerScripts)
        {
            sol::object selected = sol::nil;
            if (!selectedPtr.isEmpty())
                mLua.protectedCall([&](LuaUtil::LuaView& view) {
                    selected = sol::make_object(view.sol(), LObject(getId(selectedPtr)));
                });
            if (playerScripts->consoleCommand(consoleMode, command, selected))
                processed = true;
        }
        if (!processed)
            MWBase::Environment::get().getWindowManager()->printToConsole(
                "No Lua handlers for console\n", MWBase::WindowManager::sConsoleColor_Error);
    }

    LuaManager::DelayedAction::DelayedAction(LuaUtil::LuaState* state, std::function<void()> fn, std::string_view name)
        : mFn(std::move(fn))
        , mName(name)
    {
        if (Settings::lua().mLuaDebug)
            mCallerTraceback = state->debugTraceback();
        if (LuaUtil::LuaState::isProfilerEnabled())
            mScript = state->activeScriptPath();
    }

    void LuaManager::DelayedAction::apply() const
    {
        try
        {
            mFn();
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Error in DelayedAction " << mName << ": " << e.what();

            if (mCallerTraceback.empty())
                Log(Debug::Error) << "Set 'lua debug=true' in settings.cfg to enable action tracebacks";
            else
                Log(Debug::Error) << "Caller " << mCallerTraceback;
        }
    }

    void LuaManager::addAction(std::function<void()> action, std::string_view name)
    {
        if (mApplyingDelayedActions)
            throw std::runtime_error("DelayedAction is not allowed to create another DelayedAction");
        mActionQueue.emplace_back(&mLua, std::move(action), name);
    }

    void LuaManager::addTeleportPlayerAction(std::function<void()> action)
    {
        mTeleportPlayerAction = DelayedAction(&mLua, std::move(action), "TeleportPlayer");
    }

    void LuaManager::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "Lua UsedMemory", static_cast<double>(mLua.getTotalMemoryUsage()));
    }

    void LuaManager::addPhaseTime(ProfilePhase phase, std::chrono::steady_clock::time_point start)
    {
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        mPhaseAvgMs[phase] += (ms - mPhaseAvgMs[phase]) * profileAvgCoef;
    }

    void LuaManager::writeProfileReport() const
    {
        using Stats = LuaUtil::ScriptsContainer::ScriptStats;
        std::vector<Stats> stats;
        mGlobalScripts.collectStats(stats);
        std::size_t containers = 0;
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
            {
                scripts->collectStats(stats);
                ++containers;
            }
        }
        if (const LocalScripts* playerScripts
            = mPlayer.isEmpty() ? nullptr : dynamic_cast<const LocalScripts*>(mPlayer.getRefData().getLuaScripts()))
        {
            // the player is usually among the active local scripts; counted again only if not
            if (std::find_if(mActiveLocalScripts.begin(), mActiveLocalScripts.end(),
                    [&](const LuaUtil::ScriptsContainerWeakPtr& ptr) { return asLocal(ptr) == playerScripts; })
                == mActiveLocalScripts.end())
                playerScripts->collectStats(stats);
        }
        mMenuScripts.collectStats(stats);
        stats.resize(mConfiguration.size());

        std::vector<std::size_t> order(stats.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (stats[a].mAvgTimeUs != stats[b].mAvgTimeUs)
                return stats[a].mAvgTimeUs > stats[b].mAvgTimeUs;
            return stats[a].mAvgInstructionCount > stats[b].mAvgInstructionCount;
        });
        double totalUs = 0;
        double totalOps = 0;
        for (const Stats& s : stats)
        {
            totalUs += s.mAvgTimeUs;
            totalOps += s.mAvgInstructionCount;
        }

        std::ofstream out(mUserConfigPath / "lua-profile.txt", std::ios::trunc);
        if (!out)
            return;
        out << std::fixed << std::setprecision(2);
        out << "OpenMW Lua profile (averages over the last ~30 frames, rewritten every 5 seconds)";
        if (!mPlayer.isEmpty() && mPlayer.isInCell())
            out << "\nPlayer in " << mPlayer.getCell()->getCell()->getDescription();
        out << "\n"
            << containers << " objects with local scripts in the scene, Lua memory "
            << (mLua.getTotalMemoryUsage() / (1024 * 1024)) << " MB\n\n";

        const auto& p = mPhaseAvgMs;
        out << "Main thread, at the start of every frame (adds directly to the frame time):\n";
        out << std::setw(8) << p[Phase_SyncInputAndFrame]
            << " ms  input handlers, onFrame of player and menu scripts\n";
        out << std::setw(8) << p[Phase_SyncQueuedChanges]
            << " ms  applying the changes scripts queued last frame (UI, objects, stats):\n";
        std::vector<std::pair<std::string, QueuedChangeStats>> changes(
            mQueuedChangeStats.begin(), mQueuedChangeStats.end());
        std::sort(changes.begin(), changes.end(),
            [](const auto& a, const auto& b) { return a.second.mAvgMs > b.second.mAvgMs; });
        for (const auto& [name, change] : changes)
        {
            if (change.mAvgCount < 0.05f && change.mAvgMs < 0.005f)
                continue;
            out << "            " << std::setw(8) << change.mAvgMs << " ms  " << std::setw(7) << change.mAvgCount
                << " per frame  " << name << "\n";
        }
        out << "Lua thread, in parallel with drawing the frame (adds to the frame time only when it takes longer):\n";
        out << std::setw(8) << p[Phase_ObjectLists] << " ms  nearby object lists, script bookkeeping\n";
        out << std::setw(8) << p[Phase_Timers] << " ms  timers\n";
        out << std::setw(8) << p[Phase_Events] << " ms  events\n";
        out << std::setw(8) << p[Phase_EngineHandlers] << " ms  engine handlers (onActive, onActivated, ...)\n";
        out << std::setw(8) << p[Phase_LocalUpdate] << " ms  onUpdate of scripted objects\n";
        out << std::setw(8) << p[Phase_GlobalUpdate] << " ms  onUpdate of global scripts\n\n";

        out << "Per script, slowest first. time: wall-clock time in calls into the script, including the engine\n"
               "calls it makes (raycasts, object queries, ...); instr: Lua instructions; instances: objects the\n"
               "script runs on. Totals: "
            << totalUs / 1000.0 << " ms, " << static_cast<int64_t>(totalOps) << " instructions per frame.\n\n";
        out << "   time ms  share   instr/frame  instances  script  (attached to)\n";
        for (std::size_t i : order)
        {
            const Stats& s = stats[i];
            if (s.mAvgTimeUs < 1.f && s.mAvgInstructionCount < 1.f)
                break;
            const ESM::LuaScriptCfg& cfg = mConfiguration[i];
            out << std::setw(10) << s.mAvgTimeUs / 1000.f << std::setw(6)
                << static_cast<int>(totalUs > 0 ? 100.0 * s.mAvgTimeUs / totalUs + 0.5 : 0.0) << "%" << std::setw(14)
                << static_cast<int64_t>(s.mAvgInstructionCount) << std::setw(11) << s.mInstances << "  "
                << cfg.mScriptPath.value() << "  (";
            const char* separator = "";
            auto flag = [&](ESM::LuaScriptCfg::Flags f, const char* name) {
                if (cfg.mFlags & f)
                {
                    out << separator << name;
                    separator = " ";
                }
            };
            flag(ESM::LuaScriptCfg::sGlobal, "GLOBAL");
            flag(ESM::LuaScriptCfg::sMenu, "MENU");
            flag(ESM::LuaScriptCfg::sPlayer, "PLAYER");
            flag(ESM::LuaScriptCfg::sCustom, "CUSTOM");
            for (uint32_t type : cfg.mTypes)
            {
                std::string name;
                for (int b = 0; b < 4; ++b)
                {
                    const char c = static_cast<char>((type >> (8 * b)) & 0xff);
                    if (c != '_' && c != '\0')
                        name += c;
                }
                out << separator << name;
                separator = " ";
            }
            if (!cfg.mRecords.empty() || !cfg.mRefs.empty())
                out << separator << "specific objects";
            out << ")\n";
        }

        // OpenMGE XE: who holds the Lua memory. A large heap makes every garbage collection cycle longer, and its
        // last (atomic) step cannot be split up, so the scripts holding most of it are the ones to slim down.
        std::vector<std::pair<uint64_t, std::size_t>> memory;
        uint64_t attributed = 0;
        for (std::size_t i = 0; i < mConfiguration.size(); ++i)
        {
            const uint64_t bytes = mLua.getMemoryUsageByScriptIndex(static_cast<unsigned>(i));
            attributed += bytes;
            if (bytes >= 1024 * 1024)
                memory.emplace_back(bytes, i);
        }
        std::sort(memory.begin(), memory.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        out << "\nLua memory by script, largest first (allocations of more than "
            << Settings::lua().mSmallAllocMaxSize.get() << " bytes; smaller ones are not counted per script):\n";
        for (std::size_t n = 0; n < memory.size() && n < 25; ++n)
            out << std::setw(10) << static_cast<double>(memory[n].first) / (1024 * 1024) << " MB  "
                << mConfiguration[memory[n].second].mScriptPath.value() << "\n";
        out << std::setw(10) << static_cast<double>(attributed) / (1024 * 1024) << " MB  counted per script, "
            << static_cast<double>(mLua.getSmallAllocMemoryUsage()) / (1024 * 1024) << " MB in smaller allocations\n";
    }

    std::string LuaManager::formatResourceUsageStats() const
    {
        if (!LuaUtil::LuaState::isProfilerEnabled())
            return "Lua profiler is disabled";

        std::stringstream out;

        constexpr unsigned nameW = 50;
        constexpr int valueW = 12;

        auto outMemSize = [&](size_t bytes) {
            constexpr size_t limit = 10000;
            out << std::right << std::setw(valueW - 3);
            if (bytes < limit)
                out << bytes << " B ";
            else if (bytes < limit * 1024)
                out << (bytes / 1024) << " KB";
            else if (bytes < limit * 1024 * 1024)
                out << (bytes / (1024 * 1024)) << " MB";
            else
                out << (bytes / (1024 * 1024 * 1024)) << " GB";
        };

        const uint64_t smallAllocSize = Settings::lua().mSmallAllocMaxSize;
        out << "Total memory usage:";
        outMemSize(mLua.getTotalMemoryUsage());
        out << "\n";
        out << "LuaUtil::ScriptsContainer count: " << LuaUtil::ScriptsContainer::getInstanceCount() << "\n";
        out << "\n";
        out << "small alloc max size = " << smallAllocSize << " (section [Lua] in settings.cfg)\n";
        out << "Smaller values give more information for the profiler, but increase performance overhead.\n";
        out << "  Memory allocations <= " << smallAllocSize << " bytes:";
        outMemSize(mLua.getSmallAllocMemoryUsage());
        out << " (not tracked)\n";
        out << "  Memory allocations >  " << smallAllocSize << " bytes:";
        outMemSize(mLua.getTotalMemoryUsage() - mLua.getSmallAllocMemoryUsage());
        out << " (see the table below)\n\n";

        using Stats = LuaUtil::ScriptsContainer::ScriptStats;

        std::vector<Stats> activeStats;
        mGlobalScripts.collectStats(activeStats);
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->collectStats(activeStats);
        }

        std::vector<Stats> selectedStats;
        MWWorld::Ptr selectedPtr = MWBase::Environment::get().getWindowManager()->getConsoleSelectedObject();
        LocalScripts* selectedScripts = nullptr;
        if (!selectedPtr.isEmpty())
        {
            selectedScripts = selectedPtr.getRefData().getLuaScripts();
            if (selectedScripts)
                selectedScripts->collectStats(selectedStats);
            out << "Profiled object (selected in the in-game console): " << selectedPtr.toString() << "\n";
        }
        else
            out << "No selected object. Use the in-game console to select an object for detailed profile.\n";
        out << "\n";

        out << "Legend\n";
        out << "  ops:        Averaged number of Lua instruction per frame;\n";
        out << "  memory:     Aggregated size of Lua allocations > " << smallAllocSize << " bytes;\n";
        out << "  [all]:      Sum over all instances of each script;\n";
        out << "  [active]:   Sum over all active (i.e. currently in scene) instances of each script;\n";
        out << "  [inactive]: Sum over all inactive instances of each script;\n";
        out << "  [for selected object]: Only for the object that is selected in the console;\n";
        out << "\n";

        out << std::left;
        out << " " << std::setw(nameW + 2) << "*** Resource usage per script";
        out << std::right;
        out << std::setw(valueW) << "ops";
        out << std::setw(valueW) << "memory";
        out << std::setw(valueW) << "memory";
        out << std::setw(valueW) << "ops";
        out << std::setw(valueW) << "memory";
        out << "\n";
        out << std::left << " " << std::setw(nameW + 2) << "[name]" << std::right;
        out << std::setw(valueW) << "[all]";
        out << std::setw(valueW) << "[active]";
        out << std::setw(valueW) << "[inactive]";
        out << std::setw(valueW * 2) << "[for selected object]";
        out << "\n";

        for (size_t i = 0; i < mConfiguration.size(); ++i)
        {
            bool isGlobal = mConfiguration[i].mFlags & ESM::LuaScriptCfg::sGlobal;
            bool isMenu = mConfiguration[i].mFlags & ESM::LuaScriptCfg::sMenu;

            out << std::left;
            out << " " << std::setw(nameW) << mConfiguration[i].mScriptPath.value();
            if (mConfiguration[i].mScriptPath.value().size() > nameW)
                out << "\n " << std::setw(nameW) << ""; // if path is too long, break line
            out << std::right;
            out << std::setw(valueW) << static_cast<int64_t>(activeStats[i].mAvgInstructionCount);
            outMemSize(static_cast<size_t>(activeStats[i].mMemoryUsage));
            outMemSize(mLua.getMemoryUsageByScriptIndex(static_cast<unsigned>(i))
                - static_cast<uint64_t>(activeStats[i].mMemoryUsage));

            if (isGlobal)
                out << std::setw(valueW * 2) << "NA (global script)";
            else if (isMenu && (!selectedScripts || !selectedScripts->hasScript(static_cast<int>(i))))
                out << std::setw(valueW * 2) << "NA (menu script)";
            else if (selectedPtr.isEmpty())
                out << std::setw(valueW * 2) << "NA (not selected) ";
            else if (!selectedScripts || !selectedScripts->hasScript(static_cast<int>(i)))
                out << std::setw(valueW * 2) << "NA";
            else
            {
                out << std::setw(valueW) << static_cast<int64_t>(selectedStats[i].mAvgInstructionCount);
                outMemSize(static_cast<size_t>(selectedStats[i].mMemoryUsage));
            }
            out << "\n";
        }

        return out.str();
    }
}
