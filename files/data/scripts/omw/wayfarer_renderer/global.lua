-- Wayfarer Map Renderer (this build only): moves the player through the world for the player script, which has
-- the game draw each place's local map and write it as a picture.

local core = require('openmw.core')
local util = require('openmw.util')
local world = require('openmw.world')

local CELL = 8192

-- Outside: one stop for every 3 x 3 cells (the scene holds the cells around the player); inside: every interior.
local function plan()
    local stops, seen, interiors = {}, {}, {}
    for _, c in ipairs(world.cells) do
        if c.isExterior then
            local sx = math.floor((c.gridX + 1) / 3) * 3
            local sy = math.floor((c.gridY + 1) / 3) * 3
            local key = sx .. ',' .. sy
            local stop = seen[key]
            if not stop then
                stop = { x = sx, y = sy, cells = {} }
                seen[key] = stop
                stops[#stops + 1] = stop
            end
            stop.cells[#stop.cells + 1] = { c.gridX, c.gridY }
        else
            interiors[#interiors + 1] = c.id
        end
    end
    table.sort(stops, function(a, b)
        if a.y ~= b.y then
            return a.y > b.y
        end
        return a.x < b.x
    end)
    table.sort(interiors)
    return stops, interiors
end

return {
    eventHandlers = {
        WMR_Plan = function(d)
            local stops, interiors = plan()
            d.player:sendEvent('WMR_List', { stops = stops, interiors = interiors })
        end,
        WMR_Go = function(d)
            local p = d.player
            if d.x then
                local pos = util.vector3(d.x * CELL + CELL / 2, d.y * CELL + CELL / 2, 0)
                local cell = world.getExteriorCell(d.x, d.y)
                local h = 0
                pcall(function()
                    h = core.land.getHeightAt(pos, cell) or 0
                end)
                p:teleport(cell, util.vector3(pos.x, pos.y, math.max(h, 0) + 300))
            else
                p:teleport(d.id, util.vector3(0, 0, 0))
            end
        end,
    },
}
