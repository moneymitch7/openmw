-- Wayfarer Map Renderer (this build only): goes through every outside cell (3 x 3 at a time) and every interior,
-- has the game draw its local map and writes the pictures (see ui._wmr* in this build). Places already drawn are
-- skipped, so a run that was stopped carries on where it was. At the end it makes the zoomed-out overview pictures
-- and closes the game.

local core = require('openmw.core')
local self = require('openmw.self')
local types = require('openmw.types')
local ui = require('openmw.ui')
local I = require('openmw.interfaces')

if not ui._wmrStart then
    return {}
end

local SETTLE_FRAMES = 12 -- frames after arriving before the maps are drawn (for things to finish loading)
local SETTLE_TIME = 0.25
local WRITE_TIMEOUT = 5

local run = nil -- { stops, interiors, phase = 'ext'|'int'|'end', i, step, t, frames, done, total }

local function safeName(id)
    return (id:lower():gsub('[^%w]', '_'))
end

local function localFile(x, y)
    return string.format('textures/wayfarers_journal/local/(%d,%d).png', x, y)
end

local function stopDone(stop)
    for _, c in ipairs(stop.cells) do
        if not ui._wmrHas(localFile(c[1], c[2])) then
            return false
        end
    end
    return true
end

local function interiorDone(id)
    return ui._wmrHas('textures/wayfarers_journal/interior/' .. safeName(id) .. '/map.txt')
end

local function message(text)
    ui.showMessage('Wayfarer Map Renderer: ' .. text)
end

local function arrived(target)
    local cell = self.cell
    if not cell then
        return false
    end
    if target.x then
        return cell.isExterior and cell.gridX == target.x and cell.gridY == target.y
    end
    return cell.id:lower() == target.id:lower()
end

-- the next place not yet drawn, or nil when all are
local function nextTarget()
    while true do
        if run.phase == 'ext' then
            run.i = run.i + 1
            local stop = run.stops[run.i]
            if not stop then
                run.phase, run.i = 'int', 0
            elseif not stopDone(stop) then
                return { x = stop.x, y = stop.y }
            else
                run.done = run.done + 1
            end
        elseif run.phase == 'int' then
            run.i = run.i + 1
            local id = run.interiors[run.i]
            if not id then
                run.phase = 'end'
                return nil
            elseif not interiorDone(id) then
                return { id = id }
            else
                run.done = run.done + 1
            end
        else
            return nil
        end
    end
end

local function keepSafe()
    -- nothing should end the run: no menus left open, no dying
    if I.UI.getMode() then
        I.UI.setMode()
    end
    local health = types.Actor.stats.dynamic.health(self)
    if health.base < 100000 then
        health.base = 100000
    end
    health.current = health.base
end

local function step()
    local now = core.getRealTime()
    if run.step == 'go' then
        run.target = nextTarget()
        if not run.target then
            run.step = 'finish'
            run.t = now
            ui._wmrOverviews()
            message('making the overview pictures...')
            return
        end
        local d = { player = self.object }
        if run.target.x then
            d.x, d.y = run.target.x, run.target.y
        else
            d.id = run.target.id
        end
        core.sendGlobalEvent('WMR_Go', d)
        run.step, run.t, run.frames = 'arrive', now, 0
    elseif run.step == 'arrive' then
        if arrived(run.target) then
            run.frames = run.frames + 1
            if run.frames >= SETTLE_FRAMES and now - run.t > SETTLE_TIME then
                ui._wmrCapture()
                run.step, run.t, run.frames = 'write', now, 0
            end
        elseif now - run.t > 15 then
            -- never got there (a cell that can't be entered): skip it
            print('Wayfarer Map Renderer: could not reach ' .. tostring(run.target.id or (run.target.x .. ',' .. run.target.y)))
            run.step = 'go'
        end
    elseif run.step == 'write' then
        run.frames = run.frames + 1
        if run.frames >= 3 then
            ui._wmrWrite()
            if ui._wmrPending() == 0 or now - run.t > WRITE_TIMEOUT then
                run.done = run.done + 1
                if run.done % 20 == 0 then
                    message(string.format('%d of %d places drawn', run.done, run.total))
                end
                run.step = 'go'
            end
        end
    elseif run.step == 'finish' then
        if now - run.t > 3 then
            message('all done. The pictures are in ' .. ui._wmrDir())
            run.step = 'quit'
            run.t = now
        end
    elseif run.step == 'quit' then
        if now - run.t > 3 then
            core.quit()
        end
    end
end

local started = false

return {
    engineHandlers = {
        onFrame = function()
            if not started then
                started = true
                ui._wmrStart()
                core.sendGlobalEvent('WMR_Plan', { player = self.object })
                return
            end
            if run then
                keepSafe()
                step()
            end
        end,
    },
    eventHandlers = {
        WMR_List = function(d)
            run = {
                stops = d.stops, interiors = d.interiors, phase = 'ext', i = 0, step = 'go', done = 0,
                total = #d.stops + #d.interiors,
            }
            message(string.format('%d stops outside and %d interiors to draw', #d.stops, #d.interiors))
        end,
    },
}
