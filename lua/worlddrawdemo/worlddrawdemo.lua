--[[
    worlddrawdemo - example addon for worlddraw.

    Shows what the library gives you: geometry placed in world coordinates, a
    picture standing in the world, a mesh described once and then moved with a
    single call a frame, and all of it occluded by terrain instead of floating
    on top of it.

      //wdd here     put the demo where you are standing
      //wdd follow   make it follow you around
      //wdd spin     turn the mesh in place
      //wdd clear    remove it
      //wdd why      print what the library has to say for itself

    Note what is NOT here: no unload handler and no per-frame tick. worlddraw
    closes its handles and ticks itself, because forgetting the first crashes
    the client. The prerender handler below is this addon's own logic.

    Everything is described in Windower's coordinates: x east/west, y
    north/south, z height with NEGATIVE being up. Distances are yalms.
]]

_addon.name = 'worlddrawdemo'
_addon.author = 'Geno'
_addon.version = '0.2'
_addon.commands = {'worlddrawdemo', 'wdd'}

local addon_path = windower.addon_path:gsub('\\', '/')

local ok, wd = pcall(require, 'libs.worlddraw')
if not ok then
    windower.add_to_chat(123, '[worlddrawdemo] could not load worlddraw: ' .. tostring(wd))
    return
end

local d = wd.new('worlddrawdemo')

local dog = d:load_texture(addon_path .. 'happy_dog.jpg')
if not dog then
    windower.add_to_chat(123, '[worlddrawdemo] happy_dog.jpg failed: '
        .. tostring(d:last_error()) .. '  path: ' .. addon_path .. 'happy_dog.jpg')
end

-- Described once, at load, in model space: a square-based pyramid a yalm
-- across with the apex up. After build() it moves with one at() call, which is
-- the whole reason meshes exist.
--
-- No texture on it: every vertex here has u,v of 0, so an image would sample a
-- single texel and come out as a flat tint over the face colors rather than as
-- a picture. The dog goes on the panel below, where it reads.
local pyramid = d:mesh()
do
    local apex = {0.0, 0.0, -0.5}
    local base = {
        {-0.5, -0.5, 0.5},
        { 0.5, -0.5, 0.5},
        { 0.5,  0.5, 0.5},
        {-0.5,  0.5, 0.5},
    }
    local sides = {0xFFFF4040, 0xFF40FF40, 0xFF4080FF, 0xFFFFD040}

    local function face(a, b, c, color)
        pyramid:tri(a[1], a[2], a[3], 0, 0,
                    b[1], b[2], b[3], 0, 0,
                    c[1], c[2], c[3], 0, 0,
                    color)
    end

    for i = 1, 4 do
        face(base[i], base[i % 4 + 1], apex, sides[i])
    end
    face(base[1], base[2], base[3], 0xFF909090)
    face(base[1], base[3], base[4], 0xFF909090)
end

-- build() puts the staged triangles on the graphics device, and at addon load
-- the game often has not made one yet -- an addon can be loaded before the
-- client is anywhere near drawing. That is a state of the world, not a mistake
-- here, so build() reports it and returns false rather than raising, and the
-- staged triangles are kept. Trying again the next time the mesh is wanted is
-- the whole of the handling it needs.
local built = pyramid:build()

local function ensure_built()
    if not built then
        built = pyramid:build()
        if not built then
            return false
        end
    end
    return true
end

local placed = nil
local follow = false
local spin = false
local facing = 0.0

local function describe(x, y, z)
    d:begin()

    d:ring(x, y, z, 10.0, 0.25, 0xFFFFAA00)

    d:pillar(x, y, z, 0.15, 3.0, 0xFF00FFFF)

    for i = 0, 3 do
        local angle = i * math.pi / 2
        d:line(x, y, z - 0.05,
               x + 10.0 * math.cos(angle), y + 10.0 * math.sin(angle), z - 0.05,
               0.08, 0x80FFFFFF)
    end

    if dog then
        d:panel(x, y, z - 1.0, 2.0, 2.0, math.pi / 2, dog)
    end

    d:commit()
end

-- Six triangles, one call: this is what the mesh bought.
local function show_mesh()
    if not ensure_built() then
        return
    end
    pyramid:at(placed[1], placed[2], placed[3] - 2.0, facing)
end

local function place(x, y, z)
    placed = {x, y, z}
    describe(x, y, z)
    show_mesh()
end

windower.register_event('prerender', function()
    if spin then
        facing = (facing + 0.04) % (2 * math.pi)
    end

    if follow then
        -- Where the model is drawn, which is not where the game says you are:
        -- get_mob_by_target runs about 0.6 yalms ahead of you while moving.
        local x, y, z = d:player_draw_position()
        if not x then
            local me = windower.ffxi.get_mob_by_target('me')
            if me and me.x then
                x, y, z = me.x, me.y, me.z
            end
        end
        if x then
            place(x, y, z)
        end
    elseif spin and placed then
        show_mesh()
    end
end)

windower.register_event('addon command', function(command)
    command = command and command:lower() or 'here'

    if command == 'clear' then
        follow = false
        spin = false
        placed = nil
        d:clear()
        if built then
            pyramid:show(false)
        end
        windower.add_to_chat(207, '[worlddrawdemo] cleared')
        return
    end

    -- The engineering strings are never chatted at a player, so this is how an
    -- author reads them. Do not ask for the player's message here: reading it
    -- takes it, and worlddraw would then never get to show it.
    if command == 'why' then
        windower.add_to_chat(207, '[worlddrawdemo] ' .. tostring(wd.version()))
        windower.add_to_chat(207, '[worlddrawdemo] mesh built: ' .. tostring(built))
        windower.add_to_chat(207, '[worlddrawdemo] last: ' .. tostring(d:last_error()))
        windower.add_to_chat(207,
            '[worlddrawdemo] engineering: ' .. tostring(d:engineering_error()))
        return
    end

    if command == 'follow' then
        follow = not follow
        windower.add_to_chat(207, '[worlddrawdemo] follow ' .. (follow and 'on' or 'off'))
        return
    end

    if command == 'spin' then
        spin = not spin
        windower.add_to_chat(207, '[worlddrawdemo] spin ' .. (spin and 'on' or 'off'))
        return
    end

    -- Standing still the drawn and logical positions agree, so this one reads
    -- the position you can print.
    local me = windower.ffxi.get_mob_by_target('me')
    if not me or not me.x then
        windower.add_to_chat(123, '[worlddrawdemo] cannot read your position')
        return
    end

    follow = false
    place(me.x, me.y, me.z)
    windower.add_to_chat(207, ('[worlddrawdemo] placed at %.1f %.1f %.1f - back up to see it')
        :format(me.x, me.y, me.z))
end)
