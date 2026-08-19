#pragma once
//
// ffxi_world_draw - draw your own 3D geometry inside Final Fantasy XI's world
// from a Windower 4 plugin.
//
// Derive from ffxi::WorldDrawPlugin, override OnWorldDraw, and emit geometry in
// world coordinates. See README.md. Licensed 0BSD; see LICENSE.
//
#include "WindowerPlugin.h"

// Define FFXI_WORLD_DRAW_IMAGE_LOADING before including this header to get
// LoadTexture(path) for png/jpg/bmp/gif/tiff. It needs GDI+, so link gdiplus:
//   ... -o myplugin.dll myplugin.cpp exports.def -lgdiplus
// Without it the header pulls in no extra dependencies and you supply pixels
// to CreateTexture yourself.
#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
#include <objidl.h>
#include <gdiplus.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ffxi {

// A screen-space vertex carrying the depth that places it in the world.
// Make these with WorldDraw::Project, then pass them to Triangle or Quad.
struct Vertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rhw = 1.0f;
    DWORD color = 0xFFFFFFFF;
    float u = 0.0f;
    float v = 0.0f;
};

// Returned by CreateTexture and LoadTexture. Zero means "no texture": drawing
// with it is untextured, which is also the default.
typedef int TextureId;

class WorldDrawPlugin;

// Handed to OnWorldDraw. Anything emitted through it is placed in the world.
class WorldDraw {
public:
    // World point -> screen vertex. False when the point is behind the camera
    // or well off screen; skip whatever geometry needed it.
    bool Project(float x, float y, float z, DWORD color, Vertex& out,
                 float u = 0.0f, float v = 0.0f) const;

    void Triangle(const Vertex& a, const Vertex& b, const Vertex& c);
    void Quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d);

    // A vertical bar at (x, y, z) rising `height` yalms, `width` yalms wide,
    // turned to face the camera.
    bool Pillar(float x, float y, float z, float width, float height, DWORD color);

    // A horizontal circle centred on (x, y, z), drawn as an upright band
    // `thickness` yalms tall. `segments` is clamped to [8, 512].
    bool Ring(float x, float y, float z, float radius, float thickness,
              DWORD color, int segments = 128);

    // A camera-facing bar between two world points, `width` yalms thick.
    bool Line(float x1, float y1, float z1, float x2, float y2, float z2,
              float width, DWORD color);

    // Ground-plane camera-right, if you want to build your own billboards.
    float CameraRightX() const { return right_x_; }
    float CameraRightY() const { return right_y_; }

    const D3DVIEWPORT8& Viewport() const { return viewport_; }

    // Draw with a texture. 0 goes back to untextured. Vertex color multiplies
    // the image, so 0xFFFFFFFF leaves it untouched and anything else tints or
    // fades it. Changing texture sends the pending batch.
    void SetTexture(TextureId id);

    // Solid geometry writes depth and hides its own back faces, so a closed
    // object looks right. Flat overlays (the default) do neither, so they stay
    // visible from both sides and never occlude each other.
    void SetSolid(bool solid);
    void SetDepthWrite(bool enabled);
    void SetCulling(bool enabled);

    // A camera-facing textured rectangle centred on the point, sized in yalms,
    // so it shrinks with distance like real geometry.
    bool Sprite(float x, float y, float z, float width, float height,
                TextureId texture, DWORD color = 0xFFFFFFFF);

    // The same, but sized in screen pixels: stays readable at any distance.
    bool ScreenSprite(float x, float y, float z, float width, float height,
                      TextureId texture, DWORD color = 0xFFFFFFFF);

    // A standing rectangle with a fixed facing: it does not turn with the
    // camera. Centred on the point, sized in yalms. `facing` is in radians and
    // is the direction the picture looks towards -- 0 faces east (+x) and the
    // angle increases counter-clockwise.
    bool Panel(float x, float y, float z, float width, float height, float facing,
               TextureId texture, DWORD color = 0xFFFFFFFF);

    // Four corners you projected yourself, with UVs already on the vertices.
    void TexturedQuad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d,
                      TextureId texture);

    // Send whatever has been emitted so far. Called for you when the batch
    // fills and again at the end of the frame; you never need to call it.
    void Flush();

private:
    friend class WorldDrawPlugin;
    WorldDrawPlugin* owner_ = nullptr;
    IDirect3DDevice8* device_ = nullptr;
    Vertex* buffer_ = nullptr;
    int capacity_ = 0;
    int count_ = 0;
    float right_x_ = 1.0f;
    float right_y_ = 0.0f;
    D3DVIEWPORT8 viewport_ {};
    TextureId texture_ = 0;
    bool depth_write_ = false;
    bool culling_ = false;
};

class WorldDrawPlugin : public PluginBase {
private:
    static constexpr int max_textures_ = 64;

    struct Texture {
        IDirect3DTexture8* texture = nullptr;
        int width = 0;
        int height = 0;
    };

public:
    void __stdcall Load(PluginManager* manager) override {
        plugin_manager_ = manager;
        AcquireDevice();
        OnLoad();
    }

    void __stdcall Unload() override {
        OnUnload();
        shutdown_device_hooks();
        release_all_textures();
    }

    // Never refuses. The plugin can always be unloaded and its DLL replaced,
    // whatever else is loaded, in any order.
    bool __stdcall IgnoreUnload() override {
        return false;
    }

    void __stdcall PostRender() override {
        frame_transforms_valid_ = false;

        if (!draw_enabled_) {
            return;
        }

        OnFrame();

        // Normally a no-op: setup happens at Open(), but a plugin front-end
        // loaded before the device exists gets another chance here.
        if (!hook_installed_ && !hook_install_failed_ && EnsureDevice()) {
            install_device_hooks();
        }
    }

protected:
    // Emit your geometry here. Called once per frame while the world is drawn.
    // Runs on the render thread: keep it cheap and never block.
    virtual void OnWorldDraw(WorldDraw& draw) = 0;

    // Optional. OnFrame runs at the frame boundary and is where per-frame work
    // belongs -- reading files, updating whatever OnWorldDraw will consume.
    virtual void OnLoad() {}
    virtual void OnUnload() {}
    virtual void OnFrame() {}

    // Optional. Gets a short message when something goes wrong. Does nothing
    // by default; override to log it however you like.
    virtual void OnError(const char* message) { (void)message; }

    void SetDrawEnabled(bool enabled) { draw_enabled_ = enabled; }

    // Front-ends that can be opened by several consumers at once (a Lua module
    // is loaded once per addon, but the image and its hooks are shared) pair
    // these. The hooks come out only when the last consumer has gone. Both the
    // count and the decision it drives are taken under the chain lock, so they
    // cannot disagree with what another module is doing to the same chain.
    void Open() {
        chain_lock();
        ++open_count_;
        chain_unlock();

        // Setup is not per-frame work: find the renderer, take the device it
        // owns, and get the hooks in. A caller should not have to have ticked
        // before any of this is usable.
        if (EnsureDevice() && !hook_installed_ && !hook_install_failed_) {
            install_device_hooks();
        }
    }

    void Close() {
        chain_lock();
        const int remaining = open_count_ > 0 ? --open_count_ : 0;
        if (remaining == 0) {
            shutdown_device_hooks();
        }
        chain_unlock();
    }

    int OpenCount() const { return open_count_; }


    // Take the hooks out if nothing chained on top, and go inert either way.
    void Shutdown() { shutdown_device_hooks(); }
    bool DrawEnabled() const { return draw_enabled_; }

    // Make a texture from pixels you already have: 32-bit BGRA, `width` * 4
    // bytes per row, top row first. Returns 0 on failure.
    TextureId CreateTexture(const void* pixels, int width, int height) {
        if (!pixels || width <= 0 || height <= 0 || !EnsureDevice()) {
            return 0;
        }

        int free_index = -1;
        for (int i = 0; i < max_textures_; ++i) {
            if (!textures_[i].texture) {
                free_index = i;
                break;
            }
        }
        if (free_index < 0) {
            report_error("texture: no free slots");
            return 0;
        }

        IDirect3DTexture8* texture = nullptr;
        if (FAILED(d3d_device_->CreateTexture(static_cast<UINT>(width), static_cast<UINT>(height),
                1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture)) || !texture) {
            report_error("texture: creation failed");
            return 0;
        }

        D3DLOCKED_RECT locked {};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
            texture->Release();
            report_error("texture: lock failed");
            return 0;
        }

        const std::uint8_t* source = static_cast<const std::uint8_t*>(pixels);
        std::uint8_t* destination = static_cast<std::uint8_t*>(locked.pBits);
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
        for (int row = 0; row < height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * locked.Pitch,
                source + static_cast<std::size_t>(row) * row_bytes, row_bytes);
        }
        texture->UnlockRect(0);

        textures_[free_index].texture = texture;
        textures_[free_index].width = width;
        textures_[free_index].height = height;
        return free_index + 1;
    }

    void ReleaseTexture(TextureId id) {
        Texture* entry = texture_entry(id);
        if (entry && entry->texture) {
            entry->texture->Release();
            entry->texture = nullptr;
            entry->width = 0;
            entry->height = 0;
        }
    }

    bool TextureSize(TextureId id, int& width, int& height) const {
        const Texture* entry = texture_entry(id);
        if (!entry || !entry->texture) {
            return false;
        }
        width = entry->width;
        height = entry->height;
        return true;
    }

#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
    // Load an image file (png, jpg, bmp, gif, tiff). Requires linking gdiplus.
    // Never call this while drawing -- decoding is slow.
    TextureId LoadTexture(const char* path) {
        if (!path) {
            return 0;
        }
        if (!EnsureDevice()) {
            report_error("texture: no graphics device available");
            return 0;
        }

        if (!gdiplus_token_) {
            Gdiplus::GdiplusStartupInput input;
            if (Gdiplus::GdiplusStartup(&gdiplus_token_, &input, nullptr) != Gdiplus::Ok) {
                report_error("texture: GDI+ startup failed");
                gdiplus_token_ = 0;
                return 0;
            }
        }

        WCHAR wide[MAX_PATH] {};
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH) == 0) {
            report_error("texture: path could not be converted");
            return 0;
        }

        Gdiplus::Bitmap bitmap(wide);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            report_error("texture: image could not be read");
            return 0;
        }

        const int width = static_cast<int>(bitmap.GetWidth());
        const int height = static_cast<int>(bitmap.GetHeight());
        if (width <= 0 || height <= 0) {
            return 0;
        }

        Gdiplus::Rect area(0, 0, width, height);
        Gdiplus::BitmapData data {};
        if (bitmap.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data)
                != Gdiplus::Ok) {
            report_error("texture: image lock failed");
            return 0;
        }

        TextureId id = 0;
        if (data.Stride == width * 4) {
            id = CreateTexture(data.Scan0, width, height);
        } else {
            // Repack to tightly packed rows first.
            std::uint8_t* packed = static_cast<std::uint8_t*>(
                HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(width) * height * 4));
            if (packed) {
                for (int row = 0; row < height; ++row) {
                    std::memcpy(packed + static_cast<std::size_t>(row) * width * 4,
                        static_cast<const std::uint8_t*>(data.Scan0)
                            + static_cast<std::size_t>(row) * data.Stride,
                        static_cast<std::size_t>(width) * 4);
                }
                id = CreateTexture(packed, width, height);
                HeapFree(GetProcessHeap(), 0, packed);
            }
        }

        bitmap.UnlockBits(&data);
        return id;
    }
#endif

    // Vertices held before the batch is sent and the buffer reused. Not a
    // limit on how much you can draw -- emit as much as you like.
    static constexpr int vertex_batch_ = 8190;

private:
    friend class WorldDraw;

    void report_error(const char* message) { OnError(message); }

    Texture* texture_entry(TextureId id) {
        if (id <= 0 || id > max_textures_) {
            return nullptr;
        }
        return &textures_[id - 1];
    }

    const Texture* texture_entry(TextureId id) const {
        if (id <= 0 || id > max_textures_) {
            return nullptr;
        }
        return &textures_[id - 1];
    }

    IDirect3DTexture8* texture_for(TextureId id) {
        Texture* entry = texture_entry(id);
        return entry ? entry->texture : nullptr;
    }

    void release_all_textures() {
        for (int i = 0; i < max_textures_; ++i) {
            if (textures_[i].texture) {
                textures_[i].texture->Release();
                textures_[i].texture = nullptr;
            }
        }

#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
        if (gdiplus_token_) {
            Gdiplus::GdiplusShutdown(gdiplus_token_);
            gdiplus_token_ = 0;
        }
#endif
    }

    // The device is fetched on demand, so callers are not required to have
    // ticked first before creating textures.
    bool EnsureDevice() {
        if (!d3d_device_) {
            ensure_resolution();
            AcquireDevice();
        }
        return d3d_device_ != nullptr;
    }

    void AcquireDevice() {
        void* device = plugin_manager_ ? plugin_manager_->GetDirect3D8Device() : nullptr;
        if (!device) {
            device = DeviceFromGame();
        }
        if (device) {
            d3d_device_ = static_cast<IDirect3DDevice8*>(device);
        }
    }

    // Without a PluginManager to ask, take the device the game itself is using.
    void* DeviceFromGame() {
        if (renderer_global_ == 0) {
            return nullptr;
        }

        std::uint32_t renderer = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer_global_), renderer) || !renderer) {
            return nullptr;
        }

        std::uint32_t device = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer) + renderer_device_offset_, device)
            || !device) {
            return nullptr;
        }

        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(device));
    }

    void DispatchWorldDraw(IDirect3DDevice8* device) {
        if (!draw_enabled_ || !device || !refresh_projection_matrices(device)) {
            return;
        }

        D3DVIEWPORT8 viewport {};
        if (FAILED(device->GetViewport(&viewport))) {
            return;
        }

        if (!begin_draw_state(device)) {
            return;
        }

        float right_x = cached_view_.m[0][0];
        float right_y = cached_view_.m[2][0];
        const float right_length = std::sqrt(right_x * right_x + right_y * right_y);
        if (right_length < 1.0e-4f) {
            right_x = 1.0f;
            right_y = 0.0f;
        } else {
            right_x /= right_length;
            right_y /= right_length;
        }

        WorldDraw draw;
        draw.owner_ = this;
        draw.device_ = device;
        draw.buffer_ = vertices_;
        draw.capacity_ = vertex_batch_;
        draw.count_ = 0;
        draw.right_x_ = right_x;
        draw.right_y_ = right_y;
        draw.viewport_ = viewport;

        OnWorldDraw(draw);
        draw.Flush();

        end_draw_state(device);
    }

    // Applied when a batch is sent, so each batch carries the state it was
    // emitted under.
    void apply_batch_state(IDirect3DDevice8* device, TextureId texture,
                           bool depth_write, bool culling) {
        IDirect3DTexture8* bound = texture_for(texture);
        device->SetTexture(0, bound);
        if (bound) {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        } else {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }

        device->SetRenderState(D3DRS_ZWRITEENABLE, depth_write ? TRUE : FALSE);
        device->SetRenderState(D3DRS_CULLMODE, culling ? D3DCULL_CCW : D3DCULL_NONE);
    }

    void submit_batch(IDirect3DDevice8* device, const Vertex* vertices, int count) {
        if (count < 3) {
            return;
        }

        submit_vertices(device, D3DPT_TRIANGLELIST,
            static_cast<UINT>(count / 3), vertices, sizeof(Vertex));
    }

    Vertex vertices_[vertex_batch_] {};
    bool draw_enabled_ = true;
    inline static int open_count_ = 0;

    typedef void (*DeviceMethod)();
    typedef HRESULT (__stdcall* ResetMethod)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT (__stdcall* SetRenderTargetMethod)(IDirect3DDevice8*, IDirect3DSurface8*, IDirect3DSurface8*);
    typedef HRESULT (__stdcall* DrawPrimitiveMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
    typedef HRESULT (__stdcall* DrawIndexedPrimitiveMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
    typedef HRESULT (__stdcall* DrawPrimitiveUPMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    typedef HRESULT (__stdcall* DrawIndexedPrimitiveUPMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
        const void*, D3DFORMAT, const void*, UINT);

    struct HookSlot {
        int slot;
        const char* name;
    };

    static HRESULT __stdcall hook_reset(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* parameters) {
        hook_armed_ = false;

        const ResetMethod original = reinterpret_cast<ResetMethod>(original_methods_[hook_index_reset_]);
        if (!original) {
            return D3DERR_INVALIDCALL;
        }

        const HRESULT result = original(device, parameters);
        if (SUCCEEDED(result)) {
            hook_armed_ = true;
        }
        return result;
    }
    static HRESULT __stdcall hook_set_render_target(IDirect3DDevice8* device,
        IDirect3DSurface8* render_target, IDirect3DSurface8* depth_stencil) {
        WorldDrawPlugin* owner = hook_owner_;
        const bool qualifies = pass_transforms_valid_;
        if (owner && hook_armed_ && !hook_drawing_ && qualifies
            && owner->is_world_pass(device)) {
            hook_drawing_ = true;
            owner->DispatchWorldDraw(device);
            hook_drawing_ = false;
        }

        pass_transforms_valid_ = false;

        const SetRenderTargetMethod original =
            reinterpret_cast<SetRenderTargetMethod>(original_methods_[hook_index_set_render_target_]);
        if (!original) {
            return D3DERR_INVALIDCALL;
        }

        return original(device, render_target, depth_stencil);
    }

    static HRESULT __stdcall hook_draw_primitive(IDirect3DDevice8* device,
        D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT primitive_count) {
        if (!hook_drawing_) {
            capture_pass_transforms(device);
        }

        const DrawPrimitiveMethod original =
            reinterpret_cast<DrawPrimitiveMethod>(original_methods_[hook_index_draw_primitive_]);
        if (!original) {
            return D3DERR_INVALIDCALL;
        }

        return original(device, primitive_type, start_vertex, primitive_count);
    }

    static HRESULT __stdcall hook_draw_indexed_primitive(IDirect3DDevice8* device,
        D3DPRIMITIVETYPE primitive_type, UINT minimum_index, UINT vertex_count,
        UINT start_index, UINT primitive_count) {
        if (!hook_drawing_) {
            capture_pass_transforms(device);
        }

        const DrawIndexedPrimitiveMethod original =
            reinterpret_cast<DrawIndexedPrimitiveMethod>(original_methods_[hook_index_draw_indexed_primitive_]);
        if (!original) {
            return D3DERR_INVALIDCALL;
        }

        return original(device, primitive_type, minimum_index, vertex_count, start_index, primitive_count);
    }

    static HRESULT __stdcall hook_draw_primitive_up(IDirect3DDevice8* device,
        D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void* vertex_data, UINT stride) {
        if (!hook_drawing_) {
            capture_pass_transforms(device);
        }

        const DrawPrimitiveUPMethod original =
            reinterpret_cast<DrawPrimitiveUPMethod>(original_methods_[hook_index_draw_primitive_up_]);
        if (!original) {
            return D3DERR_INVALIDCALL;
        }

        return original(device, primitive_type, primitive_count, vertex_data, stride);
    }

    static HRESULT __stdcall hook_draw_indexed_primitive_up(IDirect3DDevice8* device,
        D3DPRIMITIVETYPE primitive_type, UINT minimum_vertex_index, UINT vertex_count,
        UINT primitive_count, const void* index_data, D3DFORMAT index_format,
        const void* vertex_data, UINT stride) {
        if (!hook_drawing_) {
            capture_pass_transforms(device);
        }

        const DrawIndexedPrimitiveUPMethod original =
            reinterpret_cast<DrawIndexedPrimitiveUPMethod>(original_methods_[hook_index_draw_indexed_primitive_up_]);
        if (!original) {
            return D3DERR_INVALIDCALL;
        }

        return original(device, primitive_type, minimum_vertex_index, vertex_count, primitive_count,
            index_data, index_format, vertex_data, stride);
    }
    static void capture_pass_transforms(IDirect3DDevice8* device) {
        if (pass_transforms_valid_ || !device) {
            return;
        }

        if (FAILED(device->GetTransform(D3DTS_VIEW, &pass_view_))
            || FAILED(device->GetTransform(D3DTS_PROJECTION, &pass_projection_))) {
            return;
        }

        pass_transforms_valid_ = true;
    }

    static DeviceMethod hook_thunk_for(int index) {
        switch (index) {
        case hook_index_reset_:
            return reinterpret_cast<DeviceMethod>(&hook_reset);
        case hook_index_set_render_target_:
            return reinterpret_cast<DeviceMethod>(&hook_set_render_target);
        case hook_index_draw_primitive_:
            return reinterpret_cast<DeviceMethod>(&hook_draw_primitive);
        case hook_index_draw_indexed_primitive_:
            return reinterpret_cast<DeviceMethod>(&hook_draw_indexed_primitive);
        case hook_index_draw_primitive_up_:
            return reinterpret_cast<DeviceMethod>(&hook_draw_primitive_up);
        case hook_index_draw_indexed_primitive_up_:
            return reinterpret_cast<DeviceMethod>(&hook_draw_indexed_primitive_up);
        default:
            break;
        }

        return nullptr;
    }

    bool is_executable_code(std::uintptr_t address) const {
        if (address == 0) {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) {
            return false;
        }

        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            return false;
        }

        const DWORD base = mbi.Protect & 0xFF;
        return base == PAGE_EXECUTE ||
            base == PAGE_EXECUTE_READ ||
            base == PAGE_EXECUTE_READWRITE ||
            base == PAGE_EXECUTE_WRITECOPY;
    }

    // Shared across every plugin built on this header, so each can detach
    // itself at any time and in any order without leaving anything behind
    // that refers to an unloaded image.
    static constexpr int hook_slot_count_ = 6;
    static constexpr int max_chained_plugins_ = 16;
    static constexpr DWORD chain_magic_ = 0x46574452;
    static constexpr DWORD chain_version_ = 1;
    static constexpr DWORD chain_drain_ms_ = 48;
    static constexpr const char* chain_mapping_name_ = "Local\\ffxi_world_draw_chain_v1";
    static constexpr const char* chain_mutex_name_ = "Local\\ffxi_world_draw_lock_v1";

    struct ChainEntry {
        void* thunk;
        void** original_slot;
    };

    struct ChainSlot {
        int count;
        ChainEntry entries[max_chained_plugins_];
    };

    struct ChainTable {
        DWORD magic;
        DWORD version;
        ChainSlot slots[hook_slot_count_];
    };

    static ChainTable* chain_table() {
        if (chain_table_) {
            return chain_table_;
        }

        HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(ChainTable), chain_mapping_name_);
        if (!mapping) {
            return nullptr;
        }

        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ChainTable));
        if (!view) {
            CloseHandle(mapping);
            return nullptr;
        }

        ChainTable* table = static_cast<ChainTable*>(view);
        if (created) {
            std::memset(table, 0, sizeof(ChainTable));
            table->magic = chain_magic_;
            table->version = chain_version_;
        } else if (table->magic != chain_magic_ || table->version != chain_version_) {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            return nullptr;
        }

        chain_mapping_ = mapping;
        chain_table_ = table;
        return chain_table_;
    }

    static HANDLE chain_lock() {
        if (!chain_mutex_) {
            chain_mutex_ = CreateMutexA(nullptr, FALSE, chain_mutex_name_);
        }
        if (chain_mutex_) {
            WaitForSingleObject(chain_mutex_, 5000);
        }
        return chain_mutex_;
    }

    static void chain_unlock() {
        if (chain_mutex_) {
            ReleaseMutex(chain_mutex_);
        }
    }

    // Register this plugin, recording enough to undo it later.
    bool chain_install(DeviceMethod* vtable, int index) {
        ChainTable* table = chain_table();
        if (!table) {
            return false;
        }

        const int slot = hook_slots_[index].slot;
        const DeviceMethod thunk = hook_thunk_for(index);
        bool ok = false;

        chain_lock();
        ChainSlot& chain = table->slots[index];
        if (chain.count < max_chained_plugins_
            && patch_vtable_slot(vtable, slot, thunk, original_methods_[index])) {
            chain.entries[chain.count].thunk = reinterpret_cast<void*>(thunk);
            chain.entries[chain.count].original_slot =
                reinterpret_cast<void**>(&original_methods_[index]);
            ++chain.count;
            ok = true;
        }
        chain_unlock();
        return ok;
    }

    // Detach this plugin, wherever it sits in the order, repairing whatever
    // referred to it so the rest keep working.
    void chain_remove(DeviceMethod* vtable, int index) {
        ChainTable* table = chain_table();
        if (!table) {
            return;
        }

        const int slot = hook_slots_[index].slot;
        void* thunk = reinterpret_cast<void*>(hook_thunk_for(index));
        void* original = reinterpret_cast<void*>(original_methods_[index]);

        chain_lock();
        ChainSlot& chain = table->slots[index];

        bool repaired = false;
        for (int i = 0; i < chain.count; ++i) {
            void** downstream = chain.entries[i].original_slot;
            if (downstream && *downstream == thunk) {
                *downstream = original;
                repaired = true;
                break;
            }
        }

        if (!repaired && vtable && vtable[slot] == reinterpret_cast<DeviceMethod>(thunk)) {
            DeviceMethod previous = nullptr;
            patch_vtable_slot(vtable, slot, reinterpret_cast<DeviceMethod>(original), previous);
        }

        for (int i = 0; i < chain.count; ++i) {
            if (chain.entries[i].thunk == thunk) {
                for (int j = i; j + 1 < chain.count; ++j) {
                    chain.entries[j] = chain.entries[j + 1];
                }
                --chain.count;
                break;
            }
        }
        chain_unlock();
    }

    bool install_device_hooks() {
        if (hook_installed_) {
            return true;
        }

        IDirect3DDevice8* device = d3d_device_;
        if (!device) {
            return false;
        }

        DeviceMethod* vtable = read_device_vtable(device);
        if (!vtable) {
            report_error("hook: device vtable unreadable");
            hook_install_failed_ = true;
            return false;
        }

        hook_vtable_ = vtable;

        int patched = 0;
        for (int index = 0; index < hook_slot_count_; ++index) {
            if (!chain_install(vtable, index)) {
                break;
            }
            hook_slot_patched_[index] = true;
            ++patched;
        }

        if (patched != hook_slot_count_) {
            for (int index = 0; index < patched; ++index) {
                chain_remove(vtable, index);
                hook_slot_patched_[index] = false;
            }
            report_error("hook: install failed, rolled back");
            hook_install_failed_ = true;
            return false;
        }

        hook_owner_ = this;
        hook_installed_ = true;
        hook_install_failed_ = false;
        hook_armed_ = true;
        return true;
    }

    void shutdown_device_hooks() {
        if (!hook_installed_ && !hook_vtable_) {
            return;
        }

        hook_armed_ = false;
        hook_owner_ = nullptr;

        DeviceMethod* vtable = hook_vtable_;
        for (int index = 0; index < hook_slot_count_; ++index) {
            if (hook_slot_patched_[index]) {
                chain_remove(vtable, index);
                hook_slot_patched_[index] = false;
            }
        }

        hook_installed_ = false;
        hook_vtable_ = nullptr;

        // Let anything already in progress finish before the image goes away.
        Sleep(chain_drain_ms_);

        if (chain_table_) {
            UnmapViewOfFile(chain_table_);
            chain_table_ = nullptr;
        }
        if (chain_mapping_) {
            CloseHandle(chain_mapping_);
            chain_mapping_ = nullptr;
        }
        if (chain_mutex_) {
            CloseHandle(chain_mutex_);
            chain_mutex_ = nullptr;
        }
    }

    DeviceMethod* read_device_vtable(IDirect3DDevice8* device) const {
        const std::uintptr_t object = reinterpret_cast<std::uintptr_t>(device);
        if (!is_readable_range(object, sizeof(DeviceMethod*))) {
            return nullptr;
        }

        DeviceMethod* vtable = *reinterpret_cast<DeviceMethod**>(object);
        if (!vtable || !is_readable_span(reinterpret_cast<std::uintptr_t>(vtable),
                sizeof(DeviceMethod) * device_vtable_slots_)) {
            return nullptr;
        }

        for (int index = 0; index < hook_slot_count_; ++index) {
            if (!is_executable_code(reinterpret_cast<std::uintptr_t>(vtable[hook_slots_[index].slot]))) {
                return nullptr;
            }
        }

        return vtable;
    }
    static bool patch_vtable_slot(DeviceMethod* vtable, int slot, DeviceMethod replacement,
        DeviceMethod& previous) {
        DWORD old_protect = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(DeviceMethod), PAGE_READWRITE, &old_protect)) {
            return false;
        }

        previous = vtable[slot];
        MemoryBarrier();
        vtable[slot] = replacement;

        DWORD ignored = 0;
        VirtualProtect(&vtable[slot], sizeof(DeviceMethod), old_protect, &ignored);
        return true;
    }

    bool is_world_pass(IDirect3DDevice8* device) {
        if (!device) {
            return false;
        }
        if (!pass_transforms_valid_ || std::fabs(pass_projection_.m[3][3]) > 1.0e-4f) {
            return false;
        }

        const float translation = std::fabs(pass_view_.m[3][0])
            + std::fabs(pass_view_.m[3][1]) + std::fabs(pass_view_.m[3][2]);
        if (translation <= 1.0e-3f) {
            return false;
        }
        const std::uintptr_t scene_depth = scene_depth_surface();
        return scene_depth != 0
            && reinterpret_cast<std::uintptr_t>(depth_surface_of(device)) == scene_depth;
    }
    std::uintptr_t scene_depth_surface() const {
        if (renderer_global_ == 0) {
            return 0;
        }

        std::uint32_t renderer = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer_global_), renderer) || renderer == 0) {
            return 0;
        }

        std::uint32_t depth = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer) + renderer_scene_depth_offset_, depth)) {
            return 0;
        }

        return depth;
    }
    static const void* depth_surface_of(IDirect3DDevice8* device) {
        IDirect3DSurface8* depth = nullptr;
        if (!device || FAILED(device->GetDepthStencilSurface(&depth)) || !depth) {
            return nullptr;
        }
        const void* identity = depth;
        depth->Release();
        return identity;
    }

    void expand_pair(float& minus_x, float& minus_y, float& plus_x, float& plus_y,
        float normal_x, float normal_y) const {
        const float delta_x = plus_x - minus_x;
        const float delta_y = plus_y - minus_y;
        const float distance = std::sqrt(delta_x * delta_x + delta_y * delta_y);
        if (distance >= min_projected_width_) {
            return;
        }

        const float center_x = (minus_x + plus_x) * 0.5f;
        const float center_y = (minus_y + plus_y) * 0.5f;
        const float half_target = min_projected_width_ * 0.5f;

        if (distance > 1.0e-3f) {
            const float scale = half_target / distance;
            minus_x = center_x - delta_x * scale;
            minus_y = center_y - delta_y * scale;
            plus_x = center_x + delta_x * scale;
            plus_y = center_y + delta_y * scale;
            return;
        }

        minus_x = center_x - normal_x * half_target;
        minus_y = center_y - normal_y * half_target;
        plus_x = center_x + normal_x * half_target;
        plus_y = center_y + normal_y * half_target;
    }

    bool refresh_projection_matrices(IDirect3DDevice8* device) {
        projection_matrices_valid_ = false;

        if (!device) {
            return false;
        }

        if (!pass_transforms_valid_) {
            return false;
        }
        if (!frame_transforms_valid_) {
            frame_view_ = pass_view_;
            frame_projection_ = pass_projection_;
            frame_transforms_valid_ = true;
        }

        cached_view_ = frame_view_;
        cached_projection_ = frame_projection_;
        if (cached_projection_.m[3][3] != 0.0f) {
            return false;
        }

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                cached_view_projection_.m[row][column] =
                    cached_view_.m[row][0] * cached_projection_.m[0][column]
                    + cached_view_.m[row][1] * cached_projection_.m[1][column]
                    + cached_view_.m[row][2] * cached_projection_.m[2][column]
                    + cached_view_.m[row][3] * cached_projection_.m[3][column];
            }
        }

        projection_matrices_valid_ = true;
        return true;
    }

    bool live_world_to_screen(float lua_x, float lua_y, float lua_z, const D3DVIEWPORT8& viewport,
        float& screen_x, float& screen_y, float& depth, float& rhw) {
        if (!projection_matrices_valid_) {
            return false;
        }

        const D3DMATRIX& view_projection = cached_view_projection_;
        const float world_x = lua_x;
        const float world_y = lua_z;
        const float world_z = lua_y;

        const float clip_x = world_x * view_projection.m[0][0] + world_y * view_projection.m[1][0]
            + world_z * view_projection.m[2][0] + view_projection.m[3][0];
        const float clip_y = world_x * view_projection.m[0][1] + world_y * view_projection.m[1][1]
            + world_z * view_projection.m[2][1] + view_projection.m[3][1];
        const float clip_z = world_x * view_projection.m[0][2] + world_y * view_projection.m[1][2]
            + world_z * view_projection.m[2][2] + view_projection.m[3][2];
        const float clip_w = world_x * view_projection.m[0][3] + world_y * view_projection.m[1][3]
            + world_z * view_projection.m[2][3] + view_projection.m[3][3];

        if (std::fabs(clip_w) <= 0.0001f) {
            return false;
        }

        const float ndc_x = clip_x / clip_w;
        const float ndc_y = clip_y / clip_w;
        if (clip_w < 0.0f || ndc_x < -4.0f || ndc_x > 4.0f || ndc_y < -4.0f || ndc_y > 4.0f) {
            return false;
        }

        screen_x = static_cast<float>(viewport.X) + (ndc_x + 1.0f) * static_cast<float>(viewport.Width) * 0.5f;
        screen_y = static_cast<float>(viewport.Y) + (1.0f - ndc_y) * static_cast<float>(viewport.Height) * 0.5f;
        const float ndc_z = clip_z / clip_w;
        const float span = viewport.MaxZ - viewport.MinZ;
        depth = viewport.MinZ + ndc_z * span;
        if (depth < 0.0f) {
            depth = 0.0f;
        } else if (depth > 1.0f) {
            depth = 1.0f;
        }
        rhw = 1.0f / clip_w;
        return true;
    }
    bool begin_draw_state(IDirect3DDevice8* device) {
        if (draw_state_active_ || !device) {
            return false;
        }

        saved_texture_ = nullptr;
        device->GetVertexShader(&saved_shader_);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &saved_alpha_);
        device->GetRenderState(D3DRS_SRCBLEND, &saved_src_);
        device->GetRenderState(D3DRS_DESTBLEND, &saved_dest_);
        device->GetRenderState(D3DRS_ZENABLE, &saved_z_);
        device->GetRenderState(D3DRS_ZWRITEENABLE, &saved_zwrite_);
        device->GetRenderState(D3DRS_ZFUNC, &saved_zfunc_);
        device->GetRenderState(D3DRS_ZBIAS, &saved_zbias_);
        device->GetRenderState(D3DRS_LIGHTING, &saved_lighting_);
        device->GetRenderState(D3DRS_CULLMODE, &saved_cull_);
        device->GetTexture(0, &saved_texture_);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &saved_alphatest_);
        device->GetRenderState(D3DRS_FOGENABLE, &saved_fog_);
        device->GetRenderState(D3DRS_STENCILENABLE, &saved_stencil_);
        device->GetTextureStageState(0, D3DTSS_COLOROP, &saved_colorop_);
        device->GetTextureStageState(0, D3DTSS_COLORARG1, &saved_colorarg1_);
        device->GetTextureStageState(0, D3DTSS_COLORARG2, &saved_colorarg2_);
        device->GetTextureStageState(0, D3DTSS_ALPHAOP, &saved_alphaop_);
        device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &saved_alphaarg1_);
        device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &saved_alphaarg2_);
        device->GetTextureStageState(1, D3DTSS_COLOROP, &saved_stage1_colorop_);

        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

        draw_state_active_ = true;
        return true;
    }

    void end_draw_state(IDirect3DDevice8* device) {
        if (!draw_state_active_ || !device) {
            return;
        }

        device->SetTexture(0, saved_texture_);
        if (saved_texture_) {
            saved_texture_->Release();
            saved_texture_ = nullptr;
        }
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, saved_alpha_);
        device->SetRenderState(D3DRS_SRCBLEND, saved_src_);
        device->SetRenderState(D3DRS_DESTBLEND, saved_dest_);
        device->SetRenderState(D3DRS_ZENABLE, saved_z_);
        device->SetRenderState(D3DRS_ZWRITEENABLE, saved_zwrite_);
        device->SetRenderState(D3DRS_ZFUNC, saved_zfunc_);
        device->SetRenderState(D3DRS_ZBIAS, saved_zbias_);
        device->SetRenderState(D3DRS_LIGHTING, saved_lighting_);
        device->SetRenderState(D3DRS_CULLMODE, saved_cull_);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, saved_alphatest_);
        device->SetRenderState(D3DRS_FOGENABLE, saved_fog_);
        device->SetRenderState(D3DRS_STENCILENABLE, saved_stencil_);
        device->SetTextureStageState(0, D3DTSS_COLOROP, saved_colorop_);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, saved_colorarg1_);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, saved_colorarg2_);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, saved_alphaop_);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, saved_alphaarg1_);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, saved_alphaarg2_);
        device->SetTextureStageState(1, D3DTSS_COLOROP, saved_stage1_colorop_);
        device->SetVertexShader(saved_shader_);
        draw_state_active_ = false;
    }

    HRESULT submit_vertices(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive_type,
        UINT primitive_count, const Vertex* vertices, UINT stride) {
        if (!device || !draw_state_active_ || primitive_count == 0 || !vertices) {
            return E_INVALIDARG;
        }

        return device->DrawPrimitiveUP(primitive_type, primitive_count, vertices, stride);
    }

    bool is_readable_page(DWORD protect) const {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS)) {
            return false;
        }

        const DWORD base = protect & 0xFF;
        return base == PAGE_READONLY ||
            base == PAGE_READWRITE ||
            base == PAGE_WRITECOPY ||
            base == PAGE_EXECUTE_READ ||
            base == PAGE_EXECUTE_READWRITE ||
            base == PAGE_EXECUTE_WRITECOPY;
    }

    bool is_readable_range(std::uintptr_t address, std::size_t size) const {
        if (address == 0 || size == 0) {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) {
            return false;
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t end = base + mbi.RegionSize;
        return mbi.State == MEM_COMMIT &&
            is_readable_page(mbi.Protect) &&
            address >= base &&
            address + size <= end;
    }
    bool is_readable_span(std::uintptr_t address, std::size_t size) const {
        if (address == 0 || size == 0) {
            return false;
        }

        const std::uintptr_t end = address + size;
        if (end < address) {
            return false;
        }

        std::uintptr_t cursor = address;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION mbi {};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) {
                return false;
            }

            const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (region_end <= cursor || mbi.State != MEM_COMMIT || !is_readable_page(mbi.Protect)) {
                return false;
            }

            cursor = region_end;
        }

        return true;
    }

    template<typename T>
    bool read_memory_raw(std::uintptr_t address, T& value) const {
        SIZE_T bytes_read = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(T), &bytes_read) &&
            bytes_read == sizeof(T);
    }

    template<typename T>
    bool read_memory(std::uintptr_t address, T& value) const {
        if (!is_readable_range(address, sizeof(T))) {
            return false;
        }

        return read_memory_raw(address, value);
    }

    bool read_bytes(std::uintptr_t address, void* output, std::size_t size) const {
        if (!output || !is_readable_span(address, size)) {
            return false;
        }

        SIZE_T bytes_read = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size, &bytes_read) &&
            bytes_read == size;
    }

    bool get_module_image(const char* name, std::uintptr_t& image_base, std::size_t& image_size) const {
        image_base = 0;
        image_size = 0;

        HMODULE module = GetModuleHandleA(name);
        if (!module) {
            return false;
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
        IMAGE_DOS_HEADER dos_header {};
        if (!read_memory(base, dos_header) || dos_header.e_magic != IMAGE_DOS_SIGNATURE
            || dos_header.e_lfanew <= 0 || dos_header.e_lfanew > 0x10000) {
            return false;
        }

        IMAGE_NT_HEADERS32 nt_headers {};
        if (!read_memory(base + static_cast<std::uintptr_t>(dos_header.e_lfanew), nt_headers)
            || nt_headers.Signature != IMAGE_NT_SIGNATURE
            || nt_headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return false;
        }

        const std::size_t size_of_image = static_cast<std::size_t>(nt_headers.OptionalHeader.SizeOfImage);
        if (size_of_image < 0x1000 || size_of_image > 0x20000000) {
            return false;
        }

        image_base = base;
        image_size = size_of_image;
        return true;
    }
    std::uintptr_t scan_span(std::uintptr_t span_begin, std::uintptr_t span_end,
        const std::uint8_t* pattern, const char* mask, std::size_t length) {
        if (length == 0 || length > scan_chunk_size_ || span_end <= span_begin
            || (span_end - span_begin) < length) {
            return 0;
        }

        std::uintptr_t position = span_begin;
        while (position + length <= span_end) {
            const std::size_t remaining = static_cast<std::size_t>(span_end - position);
            const std::size_t chunk = remaining < scan_chunk_size_ ? remaining : scan_chunk_size_;
            if (!read_bytes(position, scan_buffer_, chunk)) {
                return 0;
            }

            const std::size_t last_candidate = chunk - length;
            std::size_t cursor = 0;
            while (cursor <= last_candidate) {
                const void* found = std::memchr(scan_buffer_ + cursor, pattern[0],
                    last_candidate - cursor + 1);
                if (!found) {
                    break;
                }

                const std::size_t candidate =
                    static_cast<std::size_t>(static_cast<const std::uint8_t*>(found) - scan_buffer_);
                std::size_t offset = 1;
                for (; offset < length; ++offset) {
                    if (mask[offset] == 'x' && scan_buffer_[candidate + offset] != pattern[offset]) {
                        break;
                    }
                }

                if (offset == length) {
                    return position + candidate;
                }

                cursor = candidate + 1;
            }

            if (chunk < scan_chunk_size_) {
                break;
            }

            position += scan_chunk_size_ - (length - 1);
        }

        return 0;
    }

    std::uintptr_t scan_module(std::uintptr_t image_base, std::size_t image_size,
        const std::uint8_t* pattern, const char* mask, std::size_t length) {
        const std::uintptr_t image_end = image_base + image_size;
        std::uintptr_t cursor = image_base;

        while (cursor < image_end) {
            MEMORY_BASIC_INFORMATION mbi {};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) {
                return 0;
            }

            const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (region_end <= cursor) {
                return 0;
            }

            if (mbi.State != MEM_COMMIT || !is_readable_page(mbi.Protect)) {
                cursor = region_end;
                continue;
            }
            std::uintptr_t span_end = region_end;
            while (span_end < image_end) {
                MEMORY_BASIC_INFORMATION next {};
                if (!VirtualQuery(reinterpret_cast<const void*>(span_end), &next, sizeof(next))) {
                    break;
                }

                const std::uintptr_t next_end = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                if (next_end <= span_end || next.State != MEM_COMMIT || !is_readable_page(next.Protect)) {
                    break;
                }

                span_end = next_end;
            }

            const std::uintptr_t scan_end = span_end < image_end ? span_end : image_end;
            const std::uintptr_t hit = scan_span(cursor, scan_end, pattern, mask, length);
            if (hit) {
                return hit;
            }

            cursor = span_end;
        }

        return 0;
    }

    void ensure_resolution() {
        if (resolution_attempted_) {
            return;
        }

        resolution_attempted_ = true;
        scan_resolved_ = false;
        renderer_global_ = 0;

        std::uintptr_t image_base = 0;
        std::size_t image_size = 0;
        if (!get_module_image("FFXiMain.dll", image_base, image_size)) {
            report_error("scan: FFXiMain.dll image unavailable");
            return;
        }

        const std::uintptr_t renderer_hit = scan_module(image_base, image_size,
            renderer_pattern_, renderer_mask_, sizeof(renderer_pattern_));
        std::uint32_t renderer_global = 0;
        if (renderer_hit) {
            read_memory(renderer_hit + 2, renderer_global);
        }
        renderer_global_ = renderer_global;
        scan_resolved_ = renderer_global_ != 0;

        if (!scan_resolved_) {
            char message[256] {};
            std::snprintf(message, sizeof(message),
                "scan FAILED: module=0x%08lX size=0x%08lX renderer_hit=0x%08lX",
                static_cast<unsigned long>(image_base), static_cast<unsigned long>(image_size),
                static_cast<unsigned long>(renderer_hit));
            report_error(message);
        }
    }

    static constexpr float min_projected_width_ = 1.5f;
    static constexpr std::size_t scan_chunk_size_ = 0x10000;
    static constexpr int hook_index_reset_ = 0;
    static constexpr int hook_index_set_render_target_ = 1;
    static constexpr int hook_index_draw_primitive_ = 2;
    static constexpr int hook_index_draw_indexed_primitive_ = 3;
    static constexpr int hook_index_draw_primitive_up_ = 4;
    static constexpr int hook_index_draw_indexed_primitive_up_ = 5;
    static constexpr int device_vtable_slots_ = 97;
    static constexpr HookSlot hook_slots_[hook_slot_count_] = {
        {14, "Reset"},
        {31, "SetRenderTarget"},
        {70, "DrawPrimitive"},
        {71, "DrawIndexedPrimitive"},
        {72, "DrawPrimitiveUP"},
        {73, "DrawIndexedPrimitiveUP"},
    };
    static constexpr std::uint8_t renderer_pattern_[12] = {
        0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x89, 0xB9, 0x94, 0x01, 0x00, 0x00};
    static constexpr char renderer_mask_[13] = "xx????xxxxxx";
    static constexpr std::uintptr_t renderer_scene_depth_offset_ = 0x1A4;
    static constexpr std::uintptr_t renderer_device_offset_ = 0x0C;
    inline static WorldDrawPlugin* hook_owner_ = nullptr;
    inline static DeviceMethod* hook_vtable_ = nullptr;
    inline static DeviceMethod original_methods_[hook_slot_count_] {};
    inline static bool hook_slot_patched_[hook_slot_count_] {};
    inline static bool hook_installed_ = false;
    inline static bool hook_install_failed_ = false;
    inline static bool hook_armed_ = false;
    inline static bool hook_drawing_ = false;
    inline static ChainTable* chain_table_ = nullptr;
    inline static HANDLE chain_mapping_ = nullptr;
    inline static HANDLE chain_mutex_ = nullptr;
    inline static D3DMATRIX pass_view_ {};
    inline static D3DMATRIX pass_projection_ {};
    inline static bool pass_transforms_valid_ = false;
    inline static D3DMATRIX frame_view_ {};
    inline static D3DMATRIX frame_projection_ {};
    inline static bool frame_transforms_valid_ = false;


    IDirect3DDevice8* d3d_device_ = nullptr;
    D3DMATRIX cached_view_ {};
    D3DMATRIX cached_projection_ {};
    D3DMATRIX cached_view_projection_ {};
    bool projection_matrices_valid_ = false;
    DWORD saved_shader_ = 0;
    DWORD saved_alpha_ = 0;
    DWORD saved_src_ = 0;
    DWORD saved_dest_ = 0;
    DWORD saved_z_ = 0;
    DWORD saved_zwrite_ = 0;
    DWORD saved_zfunc_ = 0;
    DWORD saved_zbias_ = 0;
    DWORD saved_lighting_ = 0;
    DWORD saved_cull_ = 0;
    DWORD saved_alphatest_ = 0;
    DWORD saved_fog_ = 0;
    DWORD saved_stencil_ = 0;
    DWORD saved_colorop_ = 0;
    DWORD saved_colorarg1_ = 0;
    DWORD saved_colorarg2_ = 0;
    DWORD saved_alphaop_ = 0;
    DWORD saved_alphaarg1_ = 0;
    DWORD saved_alphaarg2_ = 0;
    DWORD saved_stage1_colorop_ = 0;
    IDirect3DBaseTexture8* saved_texture_ = nullptr;
    bool draw_state_active_ = false;
    bool resolution_attempted_ = false;
    bool scan_resolved_ = false;
    std::uint32_t renderer_global_ = 0;
    std::uint8_t scan_buffer_[scan_chunk_size_] {};

    Texture textures_[max_textures_] {};
#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
    ULONG_PTR gdiplus_token_ = 0;
#endif
};

inline bool WorldDraw::Project(float x, float y, float z, DWORD color, Vertex& out,
                               float u, float v) const {
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    float depth = 0.0f;
    float rhw = 1.0f;
    if (!owner_ || !owner_->live_world_to_screen(x, y, z, viewport_, screen_x, screen_y, depth, rhw)) {
        return false;
    }

    out.x = screen_x;
    out.y = screen_y;
    out.z = depth;
    out.rhw = rhw;
    out.color = color;
    out.u = u;
    out.v = v;
    return true;
}

inline void WorldDraw::Flush() {
    if (owner_ && device_ && count_ >= 3) {
        owner_->apply_batch_state(device_, texture_, depth_write_, culling_);
        owner_->submit_batch(device_, buffer_, count_);
    }
    count_ = 0;
}

inline void WorldDraw::SetTexture(TextureId id) {
    if (id != texture_) {
        Flush();
        texture_ = id;
    }
}

inline void WorldDraw::SetDepthWrite(bool enabled) {
    if (enabled != depth_write_) {
        Flush();
        depth_write_ = enabled;
    }
}

inline void WorldDraw::SetCulling(bool enabled) {
    if (enabled != culling_) {
        Flush();
        culling_ = enabled;
    }
}

inline void WorldDraw::SetSolid(bool solid) {
    SetDepthWrite(solid);
    SetCulling(solid);
}

inline void WorldDraw::TexturedQuad(const Vertex& a, const Vertex& b, const Vertex& c,
                                    const Vertex& d, TextureId texture) {
    SetTexture(texture);
    Quad(a, b, c, d);
}

inline bool WorldDraw::Sprite(float x, float y, float z, float width, float height,
                              TextureId texture, DWORD color) {
    const float half = width * 0.5f;
    const float offset_x = right_x_ * half;
    const float offset_y = right_y_ * half;

    // Centred on the point, like ScreenSprite. Negative height is up.
    const float half_height = height * 0.5f;
    const float bottom = z + half_height;
    const float top = z - half_height;

    Vertex a, b, c, d;
    if (!Project(x - offset_x, y - offset_y, bottom, color, a, 0.0f, 1.0f)
        || !Project(x + offset_x, y + offset_y, bottom, color, b, 1.0f, 1.0f)
        || !Project(x - offset_x, y - offset_y, top, color, c, 0.0f, 0.0f)
        || !Project(x + offset_x, y + offset_y, top, color, d, 1.0f, 0.0f)) {
        return false;
    }

    TexturedQuad(a, b, c, d, texture);
    return true;
}

inline bool WorldDraw::Panel(float x, float y, float z, float width, float height,
                             float facing, TextureId texture, DWORD color) {
    // The width runs across the way the panel faces.
    const float across_x = -std::sin(facing);
    const float across_y = std::cos(facing);
    const float half_width = width * 0.5f;
    const float offset_x = across_x * half_width;
    const float offset_y = across_y * half_width;

    const float half_height = height * 0.5f;
    const float bottom = z + half_height;
    const float top = z - half_height;

    Vertex a, b, c, d;
    if (!Project(x - offset_x, y - offset_y, bottom, color, a, 0.0f, 1.0f)
        || !Project(x + offset_x, y + offset_y, bottom, color, b, 1.0f, 1.0f)
        || !Project(x - offset_x, y - offset_y, top, color, c, 0.0f, 0.0f)
        || !Project(x + offset_x, y + offset_y, top, color, d, 1.0f, 0.0f)) {
        return false;
    }

    TexturedQuad(a, b, c, d, texture);
    return true;
}

inline bool WorldDraw::ScreenSprite(float x, float y, float z, float width, float height,
                                    TextureId texture, DWORD color) {
    Vertex centre;
    if (!Project(x, y, z, color, centre)) {
        return false;
    }

    const float half_w = width * 0.5f;
    const float half_h = height * 0.5f;

    Vertex a = centre, b = centre, c = centre, d = centre;
    a.x -= half_w; a.y += half_h; a.u = 0.0f; a.v = 1.0f;
    b.x += half_w; b.y += half_h; b.u = 1.0f; b.v = 1.0f;
    c.x -= half_w; c.y -= half_h; c.u = 0.0f; c.v = 0.0f;
    d.x += half_w; d.y -= half_h; d.u = 1.0f; d.v = 0.0f;

    TexturedQuad(a, b, c, d, texture);
    return true;
}

inline void WorldDraw::Triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    if (!buffer_) {
        return;
    }

    // Full batch: send it and carry on. There is no cap on how much geometry
    // a frame may contain.
    if (count_ + 3 > capacity_) {
        Flush();
    }

    buffer_[count_++] = a;
    buffer_[count_++] = b;
    buffer_[count_++] = c;
}

inline void WorldDraw::Quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
    Triangle(a, b, c);
    Triangle(c, b, d);
}

inline bool WorldDraw::Pillar(float x, float y, float z, float width, float height, DWORD color) {
    return Line(x, y, z, x, y, z - height, width, color);
}

inline bool WorldDraw::Line(float x1, float y1, float z1, float x2, float y2, float z2,
                            float width, DWORD color) {
    const float half = width * 0.5f;
    const float offset_x = right_x_ * half;
    const float offset_y = right_y_ * half;

    Vertex a, b, c, d;
    if (!Project(x1 - offset_x, y1 - offset_y, z1, color, a)
        || !Project(x1 + offset_x, y1 + offset_y, z1, color, b)
        || !Project(x2 - offset_x, y2 - offset_y, z2, color, c)
        || !Project(x2 + offset_x, y2 + offset_y, z2, color, d)) {
        return false;
    }

    if (owner_) {
        const float axis_x = c.x - a.x;
        const float axis_y = c.y - a.y;
        const float axis_length = std::sqrt(axis_x * axis_x + axis_y * axis_y);
        float normal_x = 1.0f;
        float normal_y = 0.0f;
        if (axis_length > 1.0e-3f) {
            normal_x = -axis_y / axis_length;
            normal_y = axis_x / axis_length;
        }
        owner_->expand_pair(a.x, a.y, b.x, b.y, normal_x, normal_y);
        owner_->expand_pair(c.x, c.y, d.x, d.y, normal_x, normal_y);
    }

    Quad(a, b, c, d);
    return true;
}

inline bool WorldDraw::Ring(float x, float y, float z, float radius, float thickness,
                            DWORD color, int segments) {
    if (segments < 8) {
        segments = 8;
    } else if (segments > 512) {
        segments = 512;
    }

    const float top = z - thickness;
    bool drew = false;
    Vertex previous_bottom, previous_top;
    bool have_previous = false;

    for (int i = 0; i <= segments; ++i) {
        const float angle = 6.28318530718f * static_cast<float>(i) / static_cast<float>(segments);
        const float point_x = x + radius * std::cos(angle);
        const float point_y = y + radius * std::sin(angle);

        Vertex bottom, band_top;
        const bool ok = Project(point_x, point_y, z, color, bottom)
            && Project(point_x, point_y, top, color, band_top);
        if (ok && have_previous) {
            Quad(previous_bottom, bottom, previous_top, band_top);
            drew = true;
        }

        previous_bottom = bottom;
        previous_top = band_top;
        have_previous = ok;
    }

    return drew;
}

}  // namespace ffxi
