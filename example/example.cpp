// Minimal ffxi_world_draw plugin. Draws a marker, a ring, a solid cube and
// textured sprites at a fixed spot in the world.
//
// It loads happy_dog.jpg from beside the DLL, which needs the image-loading
// switch below and gdiplus at link time (see build.sh). If the file is
// missing it falls back to a texture built in code, which needs neither.

#define FFXI_WORLD_DRAW_IMAGE_LOADING
#include "../ffxi_world_draw.h"

class ExamplePlugin final : public ffxi::WorldDrawPlugin {
public:
    const char* __stdcall GetPluginName() override { return "example"; }
    const char* __stdcall GetPluginAuthor() override { return "you"; }

    void __stdcall PluginCommand(const char* command) override {
        if (command && std::strcmp(command, "hide") == 0) {
            SetDrawEnabled(false);
        } else if (command && std::strcmp(command, "show") == 0) {
            SetDrawEnabled(true);
        }
    }

private:
    void OnFrame() override {
        // Textures need a device, so make them on a frame rather than at load.
        if (!image_) {
            char path[MAX_PATH] {};
            if (PathBesideDll("happy_dog.jpg", path, sizeof(path))) {
                image_ = LoadTexture(path);
            }
            if (!image_) {
                image_ = MakeCheckerTexture();
            }
        }
    }

    void OnWorldDraw(ffxi::WorldDraw& draw) override {
        const float x = marker_x_;
        const float y = marker_y_;
        const float z = marker_z_;

        // Flat overlay shapes: no depth writing, visible from either side.
        draw.Pillar(x, y, z, 0.15f, 3.0f, 0xFF00FFFF);
        draw.Ring(x, y, z, 10.0f, 0.25f, 0xFFFFAA00);

        // A solid box, two yalms up. Writes depth and culls its back faces so
        // it looks like an object rather than a pile of transparent sheets.
        draw.SetSolid(true);
        Box(draw, x, y, z - 2.0f, 0.5f, 0xFF3060FF);
        draw.SetSolid(false);

        // The picture as a square that always faces the camera, sized in
        // yalms, so it shrinks as you walk away.
        draw.Sprite(x, y, z - 1.0f, 2.0f, 2.0f, image_);

        // The same picture at a fixed 96 pixels: the same size on screen no
        // matter how far off it is.
        draw.ScreenSprite(x, y, z - 1.0f, 96.0f, 96.0f, image_);

        // Fixed facing: stays put while the camera moves around it.
        draw.Panel(x, y + 3.0f, z - 1.0f, 2.0f, 2.0f, 0.0f, image_);
    }

    // Eight corners, six faces, wound so the outside is what you see.
    void Box(ffxi::WorldDraw& draw, float x, float y, float z, float half, DWORD color) {
        ffxi::Vertex v[8];
        int index = 0;
        bool ok = true;
        for (int dz = 0; dz < 2; ++dz) {
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    ok = ok && draw.Project(x + (dx ? half : -half),
                                            y + (dy ? half : -half),
                                            z + (dz ? half : -half), color, v[index++]);
                }
            }
        }

        if (!ok) {
            return;
        }

        static const int faces[6][4] = {
            {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 6, 2},
            {1, 5, 3, 7}, {4, 5, 0, 1}, {2, 3, 6, 7},
        };
        for (const auto& face : faces) {
            draw.Quad(v[face[0]], v[face[1]], v[face[2]], v[face[3]]);
        }
    }

    // Assets sit next to the DLL, not wherever the game was started from.
    static bool PathBesideDll(const char* name, char* out, std::size_t size) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&PathBesideDll), &self) || !self) {
            return false;
        }

        char module_path[MAX_PATH] {};
        if (!GetModuleFileNameA(self, module_path, sizeof(module_path))) {
            return false;
        }

        char* slash = std::strrchr(module_path, '\\');
        if (!slash) {
            return false;
        }

        *slash = '\0';
        std::snprintf(out, size, "%s\\%s", module_path, name);
        return true;
    }

    // Fallback when the image file is not there.
    ffxi::TextureId MakeCheckerTexture() {
        static const int size = 32;
        std::uint32_t pixels[size * size];
        for (int row = 0; row < size; ++row) {
            for (int column = 0; column < size; ++column) {
                const bool light = ((row / 8) + (column / 8)) % 2 == 0;
                pixels[row * size + column] = light ? 0xFFFFFFFF : 0xFF202020;
            }
        }
        return CreateTexture(pixels, size, size);
    }

    ffxi::TextureId image_ = 0;
    float marker_x_ = 0.0f;
    float marker_y_ = 0.0f;
    float marker_z_ = 0.0f;
};

std::uint32_t GetInterfaceVersion() {
    return WINDOWER_INTERFACE_VERSION;
}

PluginBase* CreateInstance() {
    return new ExamplePlugin();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
