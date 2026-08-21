--[[
    lua_test - exercises lua/worlddraw.lua without a game.

    The file that ships is loaded as-is, over a fake `windower` and a fake
    native module, so what is checked is the shipping code and not a copy of
    it. The questions it answers:

      does the library say when it cannot draw, without an addon author having
      had to remember to ask?

      can anything the engine reports stop it saying so? A failed image load
      used to, for the life of the handle.

      does an addon that calls into the library every frame survive a library
      that cannot draw?

      does a message reach the player exactly once, without this file having
      to keep any record of what it has already said?

    The fake native module below reproduces the engine's own error log rather
    than standing in for it: two buffers, one for each kind of message, a
    serial counter that says which of the two is the more recent, and a
    player-facing buffer that the read empties, because reading that one takes
    it. That is the contract worlddraw.lua is written against, so a fake that
    got it wrong would be checking the wrong file.

        ./build.sh      runs this last; non-zero if a check fails
]]

local checks = 0
local failed = 0

local function check(ok, what)
    checks = checks + 1
    if not ok then
        failed = failed + 1
        print('FAIL   : ' .. what)
    end
end

local function check_equal(got, want, what)
    if got ~= want then
        print('         ' .. what)
        print('           got  ' .. tostring(got))
        print('           want ' .. tostring(want))
    end
    check(got == want, what)
end

-- The path of lua/worlddraw.lua relative to this file, which build.sh runs
-- from its own directory.
local module_path = '../../lua/worlddraw.lua'

-- A native module shaped like the DLL's. report() is the engine's OnError:
-- it sorts by the message contract -- 'worlddraw ' in front means the message
-- is for a player, anything else is an engineering string -- puts each kind in
-- its own buffer, and stamps it with the next serial.
local function make_native()
    local native = {tick_calls = 0, serial = 0, log = {}, commit_result = true}

    native.report = function(message)
        native.serial = native.serial + 1
        if message:sub(1, 10) == 'worlddraw ' then
            native.log.player = message
            native.log.player_serial = native.serial
        else
            native.log.engineering = message
            native.log.engineering_serial = native.serial
        end
    end

    native.new = function(name)
        local handle = {name = name}

        -- The most recent of either kind, one value.
        handle.last_error = function()
            local player = native.log.player_serial or 0
            local engineering = native.log.engineering_serial or 0
            if player == 0 and engineering == 0 then
                return nil
            end
            if player > engineering then
                return native.log.player
            end
            return native.log.engineering
        end

        -- Taken by the read, the way the engine hands it over: what comes
        -- back has not been shown to anybody, and the buffer has nothing to
        -- say again until the engine records another message.
        handle.player_error = function()
            local message = native.log.player
            native.log.player = nil
            native.log.player_serial = nil
            return message
        end

        -- The other kind is a record, not a handoff: it answers the same
        -- thing however many times an author asks.
        handle.engineering_error = function()
            return native.log.engineering
        end

        handle.commit = function()
            return native.commit_result
        end

        handle.close = function(self) self.closed = true end
        native.last_handle = handle
        return handle
    end

    native.tick = function() native.tick_calls = native.tick_calls + 1 end
    native.version = function() return 'worlddraw test' end
    return native
end

-- Loads a fresh copy of the module over a fresh fake windower, and hands back
-- the module, the chat it printed and the events it registered. A copy per
-- scenario, because the module keeps its handles and its poll counter.
local function load_module(native)
    local chat = {}
    local events = {}

    windower = {
        add_to_chat = function(colour, text)
            chat[#chat + 1] = {colour = colour, text = text}
        end,
        register_event = function(name, handler) events[name] = handler end,
    }

    local real_loadlib = package.loadlib
    package.loadlib = function()
        return function() return native end
    end

    local chunk = assert(loadfile(module_path))
    local ok, module = pcall(chunk)
    package.loadlib = real_loadlib
    assert(ok, module)

    return module, chat, events
end

-- The clock the module polls against. os.clock() in a client is wall time
-- since the process started; here it is whatever the frame driver below has
-- wound it to, thirty frames to the second, so a scenario says how many frames
-- it waited and the poll fires exactly where it would in a game.
local frame_rate = 30
local clock_frames = 0
os.clock = function() return clock_frames / frame_rate end

local function frames(events, count)
    for _ = 1, count do
        clock_frames = clock_frames + 1
        events.prerender()
    end
end

-- ---- 1. a setup failure reaches the player, once, in full -----------------
local missing = "worlddraw can't draw: a file is missing.\n"
    .. "Copy this addon's folder again from where you downloaded it.\n"
    .. "Missing: C:\\ffxi\\addons\\myaddon\\libs\\worlddraw_daemon.dll"

local native = make_native()
native.report(missing)

local wd, chat, events = load_module(native)
local handle = wd.new('myaddon')

check(handle ~= nil, 'the handle is returned even though the library cannot draw')
check(handle == native.last_handle, 'and it is the handle the engine made')
check_equal(#chat, 3, 'every line of the message is printed, and no more')
check_equal(chat[1].text,
    "[myaddon] worlddraw can't draw: a file is missing.",
    'line 1 carries the addon name and the first line of the message')
check_equal(chat[2].text,
    "[myaddon] Copy this addon's folder again from where you downloaded it.",
    'line 2 is the action')
check_equal(chat[3].text,
    '[myaddon] Missing: C:\\ffxi\\addons\\myaddon\\libs\\worlddraw_daemon.dll',
    'line 3 is the path it looked for')
check_equal(chat[1].colour, 123, 'printed in the error colour')
check_equal(chat[2].colour, 123, 'every line in the error colour')

-- ---- 2. a message shown is a message gone ---------------------------------
-- The read took it, so there is nothing left for the polls behind it to show
-- and nothing anywhere that has to remember what was said.
frames(events, 600)
check_equal(native.tick_calls, 600, 'the tick reaches the engine on every frame')
check_equal(#chat, 3, 'twenty seconds of polling never repeats a message')
check_equal(handle:player_error(), nil, 'and the handle has nothing left to hand over')

-- ---- 3. a message that arrives later is printed ----------------------------
local stomped = 'worlddraw stopped drawing: another program took over the graphics.\n'
    .. 'Restart FFXI, and load your addons before starting overlays like Discord or ReShade.'
native.report(stomped)

frames(events, 59)
check_equal(#chat, 3, 'the poll does not run on every frame')
frames(events, 1)
check_equal(#chat, 5, 'the new message is printed on the poll, both of its lines')
check_equal(chat[4].text,
    '[myaddon] worlddraw stopped drawing: another program took over the graphics.',
    'the runtime failure carries the name too')
check_equal(chat[5].text,
    '[myaddon] Restart FFXI, and load your addons before starting overlays like '
        .. 'Discord or ReShade.',
    'and its second line')

frames(events, 600)
check_equal(#chat, 5, 'and it is not repeated either')

-- ---- 4. engineering strings stay out of the chat --------------------------
-- The contract: a player-facing message begins with 'worlddraw ', an
-- engineering string with a subsystem prefix. Only the first kind is chatted;
-- both kinds stay retrievable through the handle.
local engineering = {
    'texture: image could not be read',
    'gpu: vertex shader could not be created',
    'gpu: device behavior flags 0x00000040, multithreaded=no',
    'hook: install failed, rolled back',
    'draw: dynamic vertex buffer unavailable, drawing through DrawPrimitiveUP',
    'scan: FFXiMain.dll image unavailable',
}

local printed_before = #chat
for _, message in ipairs(engineering) do
    native.report(message)
    frames(events, 60)
end
check_equal(#chat, printed_before,
    'not one engineering string reaches the chat, whatever its subsystem')
check_equal(native.last_handle:last_error(), engineering[#engineering],
    'while d:last_error() still holds the last of them for an author to read')
check_equal(native.last_handle:engineering_error(), engineering[#engineering],
    'and d:engineering_error() hands it back by name')

-- And a player-facing message right behind them still prints, details and all.
local with_details = "worlddraw can't draw: it failed to start.\n"
    .. 'Restart FFXI. If it happens again, please report it.\n'
    .. 'details: scan: renderer not found (module=0x10000000 size=0x00400000 hit=0x00000000)'
native.report(with_details)
frames(events, 60)
check_equal(#chat, printed_before + 3, 'a worlddraw message after them prints, all three lines')
check_equal(chat[printed_before + 1].text,
    "[myaddon] worlddraw can't draw: it failed to start.",
    'its first line')
check_equal(chat[printed_before + 3].text,
    '[myaddon] details: scan: renderer not found (module=0x10000000 size=0x00400000 '
        .. 'hit=0x00000000)',
    'and the details line goes with it, which is what makes a report useful')

-- ---- 5. success says nothing, ever ----------------------------------------
local quiet_native = make_native()
local quiet_wd, quiet_chat, quiet_events = load_module(quiet_native)
local quiet_handle = quiet_wd.new('otheraddon')

check(quiet_handle ~= nil, 'a handle that works is returned')
check_equal(#quiet_chat, 0, 'and nothing is printed for it')
frames(quiet_events, 600)
check_equal(#quiet_chat, 0, 'twenty seconds later, still nothing')
check_equal(quiet_native.tick_calls, 600, 'while the tick has been running all along')

-- ---- 6. an engineering string can no longer bury a player's message -------
-- The bug. A failed d:load_texture() parked 'texture: ...' in the one buffer a
-- handle had, the player-facing filter suppressed it, and every message meant
-- for a player after it -- the daemon missing, an ABI refusal, a stomped hook
-- -- was invisible for the life of that handle. Two buffers, and it cannot.
local buried_native = make_native()
buried_native.report('texture: image could not be read')

local buried_wd, buried_chat, buried_events = load_module(buried_native)
local buried_handle = buried_wd.new('buriedaddon')
check(buried_handle ~= nil, 'a handle whose image failed to load is still returned')
check_equal(#buried_chat, 0, 'and the failed load says nothing to the player, correctly')

buried_native.report(stomped)
frames(buried_events, 60)
check_equal(#buried_chat, 2,
    'the player-facing message behind it reaches the player, both lines')
check_equal(buried_chat[1].text,
    '[buriedaddon] worlddraw stopped drawing: another program took over the graphics.',
    'in full')

-- And it goes on working: another engineering string, then another player one.
buried_native.report('gpu: vertex shader could not be created')
frames(buried_events, 60)
check_equal(#buried_chat, 2, 'a second engineering string is still silent')
buried_native.report(missing)
frames(buried_events, 60)
check_equal(#buried_chat, 5, 'and a third player-facing message still gets through')

-- The same words are a message in their own right: nothing compares the text,
-- so a condition that comes back is said again when it comes back.
buried_native.report(missing)
frames(buried_events, 60)
check_equal(#buried_chat, 8, 'the same words reported again are shown again')

-- Shown once, and then gone. Twenty seconds of polling behind it finds an
-- empty buffer and says nothing, and so does asking the handle directly.
frames(buried_events, 600)
check_equal(#buried_chat, 8, 'a message already shown is never shown a second time')
check_equal(buried_handle:player_error(), nil, 'because the poll that showed it took it')

-- Which is why the read belongs to worlddraw.lua's poll. An addon author who
-- takes the message takes it out of the chat path, and the player is never told.
buried_native.report(stomped)
check_equal(buried_handle:player_error(), stomped, 'a read hands the message over')
check_equal(buried_handle:player_error(), nil, 'and the read after it has nothing to give')
frames(buried_events, 60)
check_equal(#buried_chat, 8, 'so the poll behind them shows nothing: the message was spent')

-- And the ordering nothing but a dedicated player-facing accessor survives:
-- a message for the player, then an engineering string on top of it, both
-- before the poll comes round. Asking for the most recent of either kind
-- hands back the engineering string, and the player never hears the other
-- one at all; asking for the player-facing one hands back what it is there
-- for, and neither kind consumes the other.
buried_native.report(stomped)
buried_native.report('texture: image lock failed')
frames(buried_events, 60)
check_equal(#buried_chat, 10,
    'a player-facing message with a later engineering string on top of it still shows')
check_equal(buried_chat[9].text,
    '[buriedaddon] worlddraw stopped drawing: another program took over the graphics.',
    'in full, both of its lines')
check_equal(buried_native.last_handle:engineering_error(), 'texture: image lock failed',
    'and the engineering string is still there behind it, unspent')

-- ---- 7. a library that cannot draw does not take the addon with it --------
-- The blocker: d:commit() used to raise when there was no device, and the
-- place an addon calls it from is a prerender handler -- an uncaught error
-- thirty times a second, burying the one line that says why nothing is drawn.
-- The engine half of this is proved in the C++ harness against the shipping
-- l_commit; what is checked here is the Lua side of the same contract.
local nodraw_native = make_native()
local nodraw_wd, nodraw_chat, nodraw_events = load_module(nodraw_native)
local nodraw = nodraw_wd.new('nodrawaddon')

nodraw_native.commit_result = false
nodraw_native.report('gpu: no graphics device, the commit published nothing')

-- What an addon's own prerender handler does: the library's tick, then a
-- describe-and-commit. Twenty seconds of it.
local commits = 0
local returned_false = 0
local raised = nil
for _ = 1, 600 do
    local ok, result = pcall(function()
        nodraw_events.prerender()
        commits = commits + 1
        return nodraw:commit()
    end)
    if not ok then
        raised = result
        break
    end
    if result == false then
        returned_false = returned_false + 1
    end
end
check(raised == nil, 'twenty seconds of committing with no device never raises')
check_equal(commits, 600, 'and the addon kept running for every one of those frames')
check_equal(returned_false, 600, 'every commit reported false rather than drawing')
check_equal(nodraw:last_error(), 'gpu: no graphics device, the commit published nothing',
    'and the reason is there for an author who asks')
check_equal(#nodraw_chat, 0,
    'while the player is never shown a line they can do nothing with')

nodraw_native.commit_result = true
check_equal(nodraw:commit(), true, 'and a commit once there is a device works: nothing is sticky')

-- ---- 8. unload closes every handle ----------------------------------------
events.unload()
check(native.last_handle.closed == true, 'unloading closes the handle the addon forgot')

print('')
print(string.format('%d checks, %d failed', checks, failed))
if failed == 0 then
    print('PASS   : lua/worlddraw.lua surfaces what the engine reports')
    os.exit(0)
end
print('FAIL')
os.exit(1)
