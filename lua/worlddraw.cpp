// worlddraw - Lua front-end for ffxi_world_draw.

#define FFXI_WORLD_DRAW_IMAGE_LOADING
#include "../ffxi_world_draw.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {

struct Command {
    enum Kind {
        Pillar,
        Ring,
        Line,
        Panel,
        Sprite,
        Triangle,
    };

    Kind kind = Pillar;
    float v[12] {};
    DWORD color = 0xFFFFFFFF;
    ffxi::TextureId texture = 0;
    int segments = 128;
};

constexpr int max_commands_ = 8192;

struct CommandList {
    Command items[max_commands_];
    int count = 0;
};

class LuaWorldDraw final : public ffxi::WorldDrawPlugin {
public:
    const char* __stdcall GetPluginName() override { return "worlddraw"; }
    const char* __stdcall GetPluginAuthor() override { return "worlddraw"; }

    void Attach() { Open(); }
    void Tick() { PostRender(); }

    void Detach() {
        Reset();
        ffxi::WorldDrawPlugin::Close();
    }

    void Begin() { building().count = 0; }

    void Add(const Command& command) {
        CommandList& list = building();
        if (list.count < max_commands_) {
            list.items[list.count++] = command;
        }
    }

    void Commit() {
        InterlockedExchange(&live_, building_);
        building_ = building_ == 0 ? 1 : 0;
        building().count = 0;
    }

    void Reset() {
        lists_[0].count = 0;
        lists_[1].count = 0;
        InterlockedExchange(&live_, -1);
    }

    ffxi::TextureId LoadImage(const char* path) {
        last_error_[0] = '\0';
        return LoadTexture(path);
    }

    const char* LastError() const { return last_error_; }

private:
    void OnError(const char* message) override {
        if (message) {
            std::snprintf(last_error_, sizeof(last_error_), "%s", message);
        }
    }

    CommandList& building() { return lists_[building_]; }

    void OnWorldDraw(ffxi::WorldDraw& draw) override {
        const LONG index = live_;
        if (index < 0 || index > 1) {
            return;
        }

        const CommandList& list = lists_[index];
        for (int i = 0; i < list.count; ++i) {
            const Command& c = list.items[i];
            switch (c.kind) {
            case Command::Pillar:
                draw.Pillar(c.v[0], c.v[1], c.v[2], c.v[3], c.v[4], c.color);
                break;
            case Command::Ring:
                draw.Ring(c.v[0], c.v[1], c.v[2], c.v[3], c.v[4], c.color, c.segments);
                break;
            case Command::Line:
                draw.Line(c.v[0], c.v[1], c.v[2], c.v[3], c.v[4], c.v[5], c.v[6], c.color);
                break;
            case Command::Panel:
                draw.Panel(c.v[0], c.v[1], c.v[2], c.v[3], c.v[4], c.v[5], c.texture, c.color);
                break;
            case Command::Sprite:
                draw.Sprite(c.v[0], c.v[1], c.v[2], c.v[3], c.v[4], c.texture, c.color);
                break;
            case Command::Triangle: {
                ffxi::Vertex a, b, d;
                if (draw.Project(c.v[0], c.v[1], c.v[2], c.color, a)
                    && draw.Project(c.v[3], c.v[4], c.v[5], c.color, b)
                    && draw.Project(c.v[6], c.v[7], c.v[8], c.color, d)) {
                    draw.Triangle(a, b, d);
                }
                break;
            }
            }
        }

        draw.SetTexture(0);
    }

    char last_error_[192] {};
    CommandList lists_[2] {};
    int building_ = 0;
    volatile LONG live_ = -1;
};

LuaWorldDraw g_draw;

float number(lua_State* L, int index) {
    return static_cast<float>(luaL_checknumber(L, index));
}

DWORD colour(lua_State* L, int index, DWORD fallback = 0xFFFFFFFF) {
    if (lua_isnoneornil(L, index)) {
        return fallback;
    }
    return static_cast<DWORD>(luaL_checknumber(L, index));
}

int l_tick(lua_State*) {
    g_draw.Tick();
    return 0;
}

int l_begin(lua_State*) {
    g_draw.Begin();
    return 0;
}

int l_commit(lua_State*) {
    g_draw.Commit();
    return 0;
}

int l_clear(lua_State*) {
    g_draw.Reset();
    return 0;
}

int l_close(lua_State*) {
    g_draw.Detach();
    return 0;
}

int l_pillar(lua_State* L) {
    Command c;
    c.kind = Command::Pillar;
    for (int i = 0; i < 5; ++i) {
        c.v[i] = number(L, i + 1);
    }
    c.color = colour(L, 6);
    g_draw.Add(c);
    return 0;
}

int l_ring(lua_State* L) {
    Command c;
    c.kind = Command::Ring;
    for (int i = 0; i < 5; ++i) {
        c.v[i] = number(L, i + 1);
    }
    c.color = colour(L, 6);
    if (!lua_isnoneornil(L, 7)) {
        c.segments = static_cast<int>(luaL_checkinteger(L, 7));
    }
    g_draw.Add(c);
    return 0;
}

int l_line(lua_State* L) {
    Command c;
    c.kind = Command::Line;
    for (int i = 0; i < 7; ++i) {
        c.v[i] = number(L, i + 1);
    }
    c.color = colour(L, 8);
    g_draw.Add(c);
    return 0;
}

int l_panel(lua_State* L) {
    Command c;
    c.kind = Command::Panel;
    for (int i = 0; i < 6; ++i) {
        c.v[i] = number(L, i + 1);
    }
    c.texture = static_cast<ffxi::TextureId>(luaL_checkinteger(L, 7));
    c.color = colour(L, 8);
    g_draw.Add(c);
    return 0;
}

int l_sprite(lua_State* L) {
    Command c;
    c.kind = Command::Sprite;
    for (int i = 0; i < 5; ++i) {
        c.v[i] = number(L, i + 1);
    }
    c.texture = static_cast<ffxi::TextureId>(luaL_checkinteger(L, 6));
    c.color = colour(L, 7);
    g_draw.Add(c);
    return 0;
}

int l_triangle(lua_State* L) {
    Command c;
    c.kind = Command::Triangle;
    for (int i = 0; i < 9; ++i) {
        c.v[i] = number(L, i + 1);
    }
    c.color = colour(L, 10);
    g_draw.Add(c);
    return 0;
}

int l_load_texture(lua_State* L) {
    const ffxi::TextureId id = g_draw.LoadImage(luaL_checkstring(L, 1));
    if (id == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_last_error(lua_State* L) {
    const char* message = g_draw.LastError();
    if (!message || message[0] == '\0') {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, message);
    }
    return 1;
}

int l_version(lua_State* L) {
    lua_pushstring(L, "worlddraw 0.2");
    return 1;
}

const luaL_Reg kFunctions[] = {
    {"tick", l_tick},
    {"begin", l_begin},
    {"commit", l_commit},
    {"clear", l_clear},
    {"close", l_close},
    {"pillar", l_pillar},
    {"ring", l_ring},
    {"line", l_line},
    {"panel", l_panel},
    {"sprite", l_sprite},
    {"triangle", l_triangle},
    {"load_texture", l_load_texture},
    {"last_error", l_last_error},
    {"version", l_version},
    {nullptr, nullptr},
};

}  // namespace

extern "C" __declspec(dllexport) int luaopen_worlddraw(lua_State* L) {
    g_draw.Attach();
    luaL_register(L, "worlddraw", kFunctions);
    return 1;
}
