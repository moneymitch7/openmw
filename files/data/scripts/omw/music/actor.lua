local AI = require("openmw.interfaces").AI
local self = require("openmw.self")
local types = require("openmw.types")
local nearby = require("openmw.nearby")

local targets = {}

-- OpenMGE XE: each actor checks its combat targets four times a second rather than every frame (it ran on every
-- actor nearby, several ms a frame in a town); actors are spread over the frames. Combat music starts at most a
-- quarter of a second later.
local CHECK_INTERVAL = 0.25
local untilCheck = math.random() * CHECK_INTERVAL

local function emitTargetsChanged()
    for _, actor in ipairs(nearby.players) do
        actor:sendEvent("OMWMusicCombatTargetsChanged", { actor = self, targets = targets })
    end
end

local function onUpdate(dt)
    -- paused: no AI runs, nothing changes
    if dt <= 0 then
        return
    end
    untilCheck = untilCheck - dt
    if untilCheck > 0 then
        return
    end
    untilCheck = untilCheck + CHECK_INTERVAL
    if untilCheck <= 0 then
        untilCheck = CHECK_INTERVAL
    end

    if types.Actor.isDeathFinished(self) or not types.Actor.isInActorsProcessingRange(self) then
        if next(targets) ~= nil then
            targets = {}
            emitTargetsChanged()
        end

        return
    end

    -- Early-out for actors without targets and without combat state when the game is not paused
    -- TODO: use events or engine handlers to detect when targets change
    local isStanceNothing = types.Actor.getStance(self) == types.Actor.STANCE.Nothing
    if isStanceNothing and next(targets) == nil and not AI.isFleeing() then
        return
    end

    local newTargets = AI.getTargets("Combat")

    local changed = false
    if #newTargets ~= #targets then
        changed = true
    else
        for i, target in ipairs(targets) do
            if target ~= newTargets[i] then
                changed = true
                break
            end
        end
    end

    targets = newTargets
    if changed then
        emitTargetsChanged()
    end
end

local function onInactive()
    if next(targets) ~= nil then
        targets = {}
        emitTargetsChanged()
    end
end

return {
    engineHandlers = {
        onUpdate = onUpdate,
        onInactive = onInactive,
    },
}
