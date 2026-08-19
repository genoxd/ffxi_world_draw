--[[
    worlddrawdemo - example addon for worlddraw.

    Shows the three things the library gives you: geometry placed in world
    coordinates, a picture standing in the world, and both of them occluded by
    terrain instead of floating on top of it.

      //wdd here     put the demo where you are standing
      //wdd follow   make it follow you around
      //wdd clear    remove it

    Everything is described in Windower's coordinates: x east/west, y
    north/south, z height with NEGATIVE being up. Distances are yalms.
]]

_addon.name = 'worlddrawdemo'
_addon.author = 'Geno'
_addon.version = '0.1'
_addon.commands = {'worlddrawdemo', 'wdd'}

local addon_path = windower.addon_path:gsub('\\', '/')
package.cpath = package.cpath .. ';' .. addon_path .. 'libs/?.dll'

local ok, wd = pcall(require, 'worlddraw')
if not ok then
    windower.add_to_chat(123, '[worlddrawdemo] could not load worlddraw: ' .. tostring(wd))
    return
end

local dog = wd.load_texture(addon_path .. 'dog.jpg')
if not dog then
    windower.add_to_chat(123, '[worlddrawdemo] dog.jpg failed: '
        .. tostring(wd.last_error()) .. '  path: ' .. addon_path .. 'dog.jpg')
end

local placed = nil
local follow = false

local function describe(x, y, z)
    wd.begin()

    wd.ring(x, y, z, 10.0, 0.25, 0xFFFFAA00)

    wd.pillar(x, y, z, 0.15, 3.0, 0xFF00FFFF)

    for i = 0, 3 do
        local angle = i * math.pi / 2
        wd.line(x, y, z - 0.05,
                x + 10.0 * math.cos(angle), y + 10.0 * math.sin(angle), z - 0.05,
                0.08, 0x80FFFFFF)
    end

    if dog then
        wd.panel(x, y, z - 1.0, 2.0, 2.0, math.pi / 2, dog)
    end

    wd.commit()
end

windower.register_event('prerender', function()
    wd.tick()

    if follow then
        local me = windower.ffxi.get_mob_by_target('me')
        if me and me.x then
            describe(me.x, me.y, me.z)
        end
    end
end)

-- Hooks must come out before Lua frees the library.
windower.register_event('unload', function()
    wd.close()
end)

windower.register_event('addon command', function(command)
    command = command and command:lower() or 'here'

    if command == 'clear' then
        follow = false
        placed = nil
        wd.clear()
        windower.add_to_chat(207, '[worlddrawdemo] cleared')
        return
    end

    if command == 'follow' then
        follow = not follow
        windower.add_to_chat(207, '[worlddrawdemo] follow ' .. (follow and 'on' or 'off'))
        return
    end

    local me = windower.ffxi.get_mob_by_target('me')
    if not me or not me.x then
        windower.add_to_chat(123, '[worlddrawdemo] cannot read your position')
        return
    end

    follow = false
    placed = {me.x, me.y, me.z}
    describe(me.x, me.y, me.z)
    windower.add_to_chat(207, ('[worlddrawdemo] placed at %.1f %.1f %.1f - back up to see it')
        :format(me.x, me.y, me.z))
end)
