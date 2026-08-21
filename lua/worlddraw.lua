--[[
    worlddraw - include this to draw your own 3D geometry inside FFXI's world.

        local wd = require('libs.worlddraw')
        local d  = wd.new('myaddon')

    Ship worlddraw.lua, worlddraw.dll and worlddraw_daemon.dll together in your
    addon's libs folder. Everything else is reached through the handle.

    This file already closes the handle on unload, ticks the library every
    frame, and tells the player when the library cannot draw. Your addon does
    not need to do any of the three. Nothing is printed while things work.

    If you do want to look at what went wrong:

        d:last_error()          the most recent message of either kind
        d:engineering_error()   the most recent internal detail, prefixed
                                'texture: ', 'gpu: ', 'hook: ', 'draw: ' or
                                'scan: '. Never shown to a player -- this is
                                what belongs in a bug report.
]]

local native
do
    local source = debug.getinfo(1, 'S').source
    local path = source:sub(1, 1) == '@' and source:sub(2) or source
    local dll = path:gsub('worlddraw%.lua$', 'worlddraw.dll')

    local loader, message = package.loadlib(dll, 'luaopen_worlddraw')
    if not loader then
        error('worlddraw: could not load ' .. dll .. ': ' .. tostring(message), 2)
    end

    native = loader()
end

local handles = {}

local worlddraw = {}

-- A chat line per line of the message, each carrying the name the addon gave
-- itself, so a player running several can tell which one is speaking. 123 is
-- Windower's error colour.
local function announce(entry, message)
    for line in message:gmatch('[^\n]+') do
        windower.add_to_chat(123, '[' .. entry.name .. '] ' .. line)
    end
end

-- The engine records why it cannot draw and says nothing itself, so this is
-- what puts the reason in front of the player. Reading the message takes it,
-- so there is nothing to remember here: what comes back has not been shown,
-- and the read after it comes back empty.
local function check(entry)
    local message = entry.handle:player_error()
    if message then
        announce(entry, message)
    end
end

function worlddraw.new(name)
    if type(name) ~= 'string' then
        error('worlddraw.new: name must be a string', 2)
    end

    local handle = native.new(name)

    local entry = {handle = handle, name = name}
    handles[#handles + 1] = entry

    -- Whatever went wrong at setup has gone wrong by now. The handle is
    -- returned either way: an addon whose library cannot draw still runs, and
    -- every call on the handle stays safe to make.
    check(entry)
    return handle
end

function worlddraw.version()
    return native.version()
end

-- Some failures arrive long after load -- another program taking the graphics
-- hooks, a device the game replaced -- so the tick looks for them every two
-- seconds. No string crosses out of C on the frames in between.
local poll_interval = 2.0
local last_poll = 0

windower.register_event('prerender', function()
    native.tick()

    local now = os.clock()
    if now - last_poll < poll_interval then
        return
    end
    last_poll = now

    for i = 1, #handles do
        check(handles[i])
    end
end)

windower.register_event('unload', function()
    for i = 1, #handles do
        local handle = handles[i].handle
        pcall(handle.close, handle)
    end
end)

return worlddraw
