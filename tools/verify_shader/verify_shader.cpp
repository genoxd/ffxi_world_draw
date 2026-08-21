// verify_shader - the proof behind the vs_1_1 tokens embedded in
// ffxi_world_draw.h.
//
// It holds the same assembly listings that sit in the comments above
// gpu_shader_function and gpu_line_function, assembles each through
// D3DXAssembleShader, strips the assembler's own leading comment block if it
// emits one, and byte-compares the result with the matching array in the
// header. Exit 0 means every shader is byte-equal; anything else prints why
// and dumps both token streams.
//
// It also reads the header back and checks that every line of every listing it
// assembled still appears there verbatim, so the copies here and the comments
// there cannot drift apart without this failing.
//
// The same assembler also GENERATES those arrays, so nothing is ever encoded
// by hand: `generate` assembles each listing and rewrites the span between the
// BEGIN/END GENERATED markers in the header with the tokens it got, then
// verifies what it wrote. The listing comment beside each array stays the
// source of truth, the header stays one self-contained file, and the loop
// "assemble, read the dump, retype the bytes" is gone.
//
//   build.sh                      builds it and verifies, under wine
//   build.sh generate             ... and rewrites the token arrays first
//   verify_shader.exe [verify|generate] [dll] [header]
//
// The assembler is looked up by name: any d3dx8 (the real thing, if one is
// installed) is called with the D3DX8 signature, anything else with the D3DX9
// one. The token format for a vs_1_1 shader is the same either way -- the
// shader model is, and both runtimes assemble it into the D3D8-era token
// stream that CreateVertexShader takes.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../ffxi_world_draw.h"

#include <cstdio>
#include <cstring>

namespace {

// ID3DXBuffer has the same shape in d3dx8 and d3dx9: IUnknown plus the two
// accessors. Declared here so this tool needs no DirectX SDK headers.
struct AsmBuffer;

struct AsmBufferVtbl {
    HRESULT (__stdcall* QueryInterface)(AsmBuffer*, const IID*, void**);
    ULONG (__stdcall* AddRef)(AsmBuffer*);
    ULONG (__stdcall* Release)(AsmBuffer*);
    void* (__stdcall* GetBufferPointer)(AsmBuffer*);
    DWORD (__stdcall* GetBufferSize)(AsmBuffer*);
};

struct AsmBuffer {
    const AsmBufferVtbl* vtbl;
};

// d3dx8: (source, length, flags, &constants, &shader, &errors)
typedef HRESULT (__stdcall* AssembleShader8)(const void*, UINT, DWORD,
    AsmBuffer**, AsmBuffer**, AsmBuffer**);

// d3dx9: (source, length, defines, include, flags, &shader, &errors)
typedef HRESULT (__stdcall* AssembleShader9)(const void*, UINT, const void*, void*, DWORD,
    AsmBuffer**, AsmBuffer**);

// The listings, one line per element, exactly as they read in the header.
const char* const billboard_lines[] = {
    "vs.1.1",
    "; r0 = anchor + up*oy; the width offset joins in clip space, below",
    "mad r0.xyz, c5.xyz, v3.y, v0.xyz",
    "mov r0.w, c7.x                      ; 1.0",
    "dp4 r1.x, r0, c0                    ; transform: c0-c3 are the COLUMNS",
    "dp4 r1.y, r0, c1                    ;   of the row-vector-convention",
    "dp4 r1.z, r0, c2                    ;   matrix (v*M), see LoadGpuConstants",
    "dp4 r1.w, r0, c3",
    "mul r2, c9, v3.x                    ; the exact width offset, in clip",
    "max r3.x, v3.x, -v3.x               ; |ox|",
    "mul r3.x, r3.x, c11.x               ; its half width in pixels at w = 1",
    "mul r3.y, c11.y, r1.w               ; the floor's half width, same units",
    "sge r3.z, r3.x, r3.y                ; 1 when the exact width is enough",
    "sge r4.x, v3.x, c7.y                ; which side of the quad this is",
    "sge r4.y, -v3.x, c7.y",
    "add r3.w, r4.x, -r4.y               ; +1, -1, and exactly 0 when ox is 0",
    "mul r4.xy, c10.xy, r1.w             ; the floored offset, in clip units",
    "mul r4.xy, r4.xy, r3.w              ; ... on this vertex's side",
    "add r5.xy, r2.xy, -r4.xy",
    "mad r2.xy, r5.xy, r3.z, r4.xy       ; pick one; z stays exact",
    "mul r2.w, r2.w, r3.z                ; a floored quad keeps the anchor w",
    "add r1, r1, r2",
    "mul r5.xy, v3.zw, c6.xy             ; pixel offsets to clip scale",
    "mad r1.xy, r5.xy, r1.w, r1.xy       ; perspective-correct screen shift",
    "mov oPos, r1",
    "mov oD0, v1",
    "mov oT0, v2"
};

const char* const line_lines[] = {
    "vs.1.1",
    "; a camera-facing line: widen across the view direction and the line",
    "mul r0.xyz, c8.yzx, v4.zxy          ; r0 = cross(view forward, direction)",
    "mad r0.xyz, -c8.zxy, v4.yzx, r0.xyz",
    "dp3 r1.x, r0, r0                    ; normalise it",
    "rsq r1.x, r1.x",
    "mul r0.xyz, r0.xyz, r1.x",
    "mad r0.xyz, r0.xyz, v3.x, v0.xyz    ; anchor + width direction * v3.x",
    "mov r0.w, c7.x                      ; 1.0",
    "dp4 r1.x, r0, c0                    ; transform: c0-c3 are the COLUMNS",
    "dp4 r1.y, r0, c1                    ;   of the row-vector-convention",
    "dp4 r1.z, r0, c2                    ;   matrix (v*M), see LoadGpuConstants",
    "dp4 r1.w, r0, c3",
    "mov oPos, r1",
    "mov oD0, v1",
    "mov oT0, v2"
};

// One shader to prove: its listing, the tokens the header claims that listing
// assembles to, and the declaration that goes with it (data, not assembly, so
// it is printed for a reviewer rather than proven).
struct ShaderCase {
    const char* name;
    const char* array_name;  // the C array in the header, and its marker
    const char* const* lines;
    int line_count;
    const DWORD* tokens;
    UINT token_count;
    const DWORD* declaration;
    UINT declaration_count;
};

const ShaderCase shader_cases_[] = {
    {"billboard", "gpu_shader_function", billboard_lines,
        static_cast<int>(sizeof(billboard_lines) / sizeof(billboard_lines[0])),
        ffxi::gpu_shader_function,
        static_cast<UINT>(sizeof(ffxi::gpu_shader_function) / sizeof(DWORD)),
        ffxi::gpu_shader_declaration,
        static_cast<UINT>(sizeof(ffxi::gpu_shader_declaration) / sizeof(DWORD))},
    {"line", "gpu_line_function", line_lines,
        static_cast<int>(sizeof(line_lines) / sizeof(line_lines[0])),
        ffxi::gpu_line_function,
        static_cast<UINT>(sizeof(ffxi::gpu_line_function) / sizeof(DWORD)),
        ffxi::gpu_line_declaration,
        static_cast<UINT>(sizeof(ffxi::gpu_line_declaration) / sizeof(DWORD))},
};

constexpr int shader_case_count_ =
    static_cast<int>(sizeof(shader_cases_) / sizeof(shader_cases_[0]));

constexpr UINT max_tokens_ = 1024;

// What `generate` assembles with unless told otherwise. Wine's own d3dx9 is
// the canonical one here: it is present wherever this runs and it emits no
// banner comment, so what it returns is the shader and nothing else. A real
// d3dx8, when one is installed, is assembled with as well and the two are
// required to agree before anything is written.
const char* const canonical_assembler_ = "d3dx9_43.dll";

const char* const default_assemblers_[] = {
    "d3dx8.dll",      // the real D3DX8, when one is installed
    "d3dx8ab.dll",    // the DirectX 8.1 redistributable's name for it
    "d3dx8d.dll",     // the DirectX 8 SDK's debug build
    "d3dx9_43.dll",   // Wine's own, which is what runs here
    "d3dx9_36.dll",
};

void release_buffer(AsmBuffer* buffer) {
    if (buffer) {
        buffer->vtbl->Release(buffer);
    }
}

void dump(const char* label, const DWORD* tokens, UINT count) {
    std::printf("%s (%u dwords)\n", label, static_cast<unsigned>(count));
    for (UINT i = 0; i < count; ++i) {
        std::printf("  0x%08lX%s", static_cast<unsigned long>(tokens[i]),
            (i % 4 == 3 || i + 1 == count) ? "\n" : "");
    }
}

bool build_listing(const ShaderCase& shader, char* out, std::size_t size) {
    out[0] = '\0';
    std::size_t used = 0;
    for (int i = 0; i < shader.line_count; ++i) {
        const std::size_t length = std::strlen(shader.lines[i]);
        if (used + length + 2 >= size) {
            return false;
        }
        std::memcpy(out + used, shader.lines[i], length);
        used += length;
        out[used++] = '\n';
        out[used] = '\0';
    }
    return true;
}

// Every line of the listing must still be in the header, or the copy above has
// drifted from the comment the tokens are documented by.
bool listing_matches_header(const ShaderCase& shader, const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::printf("NOTE   : header %s could not be opened; the drift check was skipped\n", path);
        return true;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        std::printf("FAIL   : header %s is empty\n", path);
        return false;
    }

    char* text = static_cast<char*>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (!text) {
        std::fclose(file);
        std::printf("FAIL   : out of memory reading %s\n", path);
        return false;
    }

    const std::size_t read = std::fread(text, 1, static_cast<std::size_t>(size), file);
    std::fclose(file);
    text[read] = '\0';

    bool ok = true;
    for (int i = 0; i < shader.line_count; ++i) {
        if (!std::strstr(text, shader.lines[i])) {
            std::printf("FAIL   : header %s no longer contains listing line \"%s\"\n",
                path, shader.lines[i]);
            ok = false;
        }
    }

    std::free(text);
    if (ok) {
        std::printf("listing: all %d lines of the %s shader found verbatim in %s\n",
            shader.line_count, shader.name, path);
    }
    return ok;
}

// The assembler may put its banner in a comment token right after the version
// token. Comment tokens are not part of the shader, so they are dropped -- and
// exactly they are: the run of comments at the top, nothing else.
UINT strip_leading_comments(const DWORD* tokens, UINT count, DWORD* out, UINT& stripped) {
    stripped = 0;
    if (count == 0) {
        return 0;
    }

    out[0] = tokens[0];  // the version token stays
    UINT read = 1;
    while (read < count && (tokens[read] & 0x0000FFFFu) == 0x0000FFFEu) {
        const UINT length = (tokens[read] >> 16) & 0x7FFFu;
        if (stripped == 0) {
            std::printf("banner : \"%.*s\"\n", static_cast<int>(length * 4),
                reinterpret_cast<const char*>(&tokens[read + 1]));
        }
        read += 1 + length;
        stripped += 1 + length;
    }

    UINT written = 1;
    while (read < count) {
        out[written++] = tokens[read++];
    }
    return written;
}

bool assemble(const char* dll_name, const char* source, DWORD* out, UINT& out_count) {
    HMODULE module = LoadLibraryA(dll_name);
    if (!module) {
        return false;
    }

    FARPROC entry = GetProcAddress(module, "D3DXAssembleShader");
    if (!entry) {
        std::printf("NOTE   : %s has no D3DXAssembleShader\n", dll_name);
        FreeLibrary(module);
        return false;
    }

    const bool eight = std::strstr(dll_name, "d3dx8") != nullptr;
    const UINT length = static_cast<UINT>(std::strlen(source));
    AsmBuffer* shader = nullptr;
    AsmBuffer* errors = nullptr;
    HRESULT result = E_FAIL;

    // Copied rather than cast: a cast between function pointer types of
    // different signatures is exactly what -Wcast-function-type objects to,
    // and this is the one place the signature is only known by name.
    if (eight) {
        AssembleShader8 entry8 = nullptr;
        std::memcpy(&entry8, &entry, sizeof(entry8));
        AsmBuffer* constants = nullptr;
        result = entry8(source, length, 0, &constants, &shader, &errors);
        release_buffer(constants);
    } else {
        AssembleShader9 entry9 = nullptr;
        std::memcpy(&entry9, &entry, sizeof(entry9));
        result = entry9(source, length, nullptr, nullptr, 0, &shader, &errors);
    }

    std::printf("assembler: %s (%s signature) hr=0x%08lX\n", dll_name, eight ? "d3dx8" : "d3dx9",
        static_cast<unsigned long>(result));

    if (errors) {
        const char* message = static_cast<const char*>(errors->vtbl->GetBufferPointer(errors));
        if (message && message[0]) {
            std::printf("messages: %s\n", message);
        }
        release_buffer(errors);
    }

    if (FAILED(result) || !shader) {
        release_buffer(shader);
        FreeLibrary(module);
        out_count = 0;
        return false;
    }

    const DWORD size = shader->vtbl->GetBufferSize(shader);
    const DWORD* tokens = static_cast<const DWORD*>(shader->vtbl->GetBufferPointer(shader));
    const UINT count = static_cast<UINT>(size / sizeof(DWORD));
    if (!tokens || count == 0 || count > max_tokens_) {
        std::printf("FAIL   : assembler returned %lu bytes\n", static_cast<unsigned long>(size));
        release_buffer(shader);
        FreeLibrary(module);
        out_count = 0;
        return false;
    }

    std::memcpy(out, tokens, count * sizeof(DWORD));
    out_count = count;
    release_buffer(shader);

    // The DLL stays loaded: the tokens are already copied out, and unloading a
    // d3dx that is mid-teardown buys nothing here.
    return true;
}

}  // namespace

// ---- generation ------------------------------------------------------------

// Splits the token stream into instructions. In the D3D8 token format a
// parameter token has bit 31 set and an instruction token does not, so an
// instruction runs from its own token up to the next token without that bit.
// The end token 0x0000FFFF has it clear too and ends the walk.
bool instruction_spans(const DWORD* tokens, UINT count, UINT* starts, UINT* lengths,
    int capacity, int& found) {
    found = 0;
    UINT index = 1;  // token 0 is the version
    while (index < count && tokens[index] != 0x0000FFFFu) {
        if (tokens[index] & 0x80000000u) {
            std::printf("FAIL   : token %u is a parameter with no instruction\n",
                static_cast<unsigned>(index));
            return false;
        }
        if (found >= capacity) {
            std::printf("FAIL   : more instructions than this tool can hold\n");
            return false;
        }

        const UINT start = index;
        ++index;
        while (index < count && (tokens[index] & 0x80000000u)) {
            ++index;
        }

        starts[found] = start;
        lengths[found] = index - start;
        ++found;
    }

    if (index >= count || tokens[index] != 0x0000FFFFu) {
        std::printf("FAIL   : the token stream does not end where it should\n");
        return false;
    }
    return true;
}

// The listing line for an instruction, without its trailing `;` comment or the
// padding before it -- what the generated array carries above each row.
void instruction_text(const char* line, char* out, std::size_t size) {
    std::size_t length = 0;
    while (line[length] && line[length] != ';' && length + 1 < size) {
        out[length] = line[length];
        ++length;
    }
    while (length > 0 && (out[length - 1] == ' ' || out[length - 1] == '\t')) {
        --length;
    }
    out[length] = '\0';
}

#define VERIFY_SHADER_APPEND(...) \
    do { \
        const int written = std::snprintf(out + used, size - used, __VA_ARGS__); \
        if (written < 0 || static_cast<std::size_t>(written) >= size - used) { \
            std::printf("FAIL   : generated source does not fit\n"); \
            return false; \
        } \
        used += static_cast<std::size_t>(written); \
    } while (0)

// The array as C source, in the shape the header carries it: one row of tokens
// per instruction, under the line of assembly it came from.
bool build_array_source(const ShaderCase& shader, const DWORD* tokens, UINT count,
    char* out, std::size_t size) {
    UINT starts[256] {};
    UINT lengths[256] {};
    int found = 0;
    if (!instruction_spans(tokens, count, starts, lengths, 256, found)) {
        return false;
    }

    // Instruction n of the stream is instruction n of the listing: the version
    // line and the `;` lines are not instructions and are stepped over here.
    int listing_index = 1;
    std::size_t used = 0;

    VERIFY_SHADER_APPEND("inline constexpr DWORD %s[] = {\n", shader.array_name);
    VERIFY_SHADER_APPEND("    0x%08lX,  // %s\n",
        static_cast<unsigned long>(tokens[0]), shader.lines[0]);

    for (int i = 0; i < found; ++i) {
        while (listing_index < shader.line_count && shader.lines[listing_index][0] == ';') {
            ++listing_index;
        }
        if (listing_index >= shader.line_count) {
            std::printf("FAIL   : %d instructions assembled but the listing has fewer\n", found);
            return false;
        }

        char text[128] {};
        instruction_text(shader.lines[listing_index], text, sizeof(text));
        ++listing_index;

        VERIFY_SHADER_APPEND("    // %s\n   ", text);
        for (UINT token = 0; token < lengths[i]; ++token) {
            VERIFY_SHADER_APPEND(" 0x%08lX,",
                static_cast<unsigned long>(tokens[starts[i] + token]));
        }
        VERIFY_SHADER_APPEND("\n");
    }

    while (listing_index < shader.line_count && shader.lines[listing_index][0] == ';') {
        ++listing_index;
    }
    if (listing_index != shader.line_count) {
        std::printf("FAIL   : the listing has instructions the assembler did not emit\n");
        return false;
    }

    VERIFY_SHADER_APPEND("    0x%08lX,  // end\n", static_cast<unsigned long>(tokens[count - 1]));
    VERIFY_SHADER_APPEND("};\n");
    return true;
}

#undef VERIFY_SHADER_APPEND

char* read_file(const char* path, std::size_t& length) {
    length = 0;
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return nullptr;
    }
    char* text = static_cast<char*>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (!text) {
        std::fclose(file);
        return nullptr;
    }
    length = std::fread(text, 1, static_cast<std::size_t>(size), file);
    std::fclose(file);
    text[length] = '\0';
    return text;
}

// Replaces exactly the array between this shader's BEGIN and END markers. The
// markers, the listing comment above them and everything else in the header
// are left untouched, so the file stays the one self-contained header it is.
bool rewrite_generated_span(const char* path, const ShaderCase& shader, const char* block) {
    std::size_t length = 0;
    char* text = read_file(path, length);
    if (!text) {
        std::printf("FAIL   : %s could not be read\n", path);
        return false;
    }

    char begin_marker[128] {};
    char end_marker[128] {};
    std::snprintf(begin_marker, sizeof(begin_marker), "// >>> BEGIN GENERATED %s",
        shader.array_name);
    std::snprintf(end_marker, sizeof(end_marker), "// <<< END GENERATED %s", shader.array_name);

    char* begin = std::strstr(text, begin_marker);
    char* end = begin ? std::strstr(begin, end_marker) : nullptr;
    char* span = begin ? std::strstr(begin, "inline constexpr DWORD") : nullptr;
    if (!begin || !end || !span || span > end) {
        std::printf("FAIL   : %s has no array between BEGIN/END GENERATED %s\n",
            path, shader.array_name);
        std::free(text);
        return false;
    }

    FILE* file = std::fopen(path, "wb");
    if (!file) {
        std::printf("FAIL   : %s could not be written\n", path);
        std::free(text);
        return false;
    }
    std::fwrite(text, 1, static_cast<std::size_t>(span - text), file);
    std::fwrite(block, 1, std::strlen(block), file);
    std::fwrite(end, 1, length - static_cast<std::size_t>(end - text), file);
    std::fclose(file);
    std::free(text);
    return true;
}

int generate(const ShaderCase& shader, const char* forced, const char* header_path) {
    std::printf("\n==== %s shader: generate ====\n", shader.name);

    char source[4096];
    if (!build_listing(shader, source, sizeof(source))) {
        std::printf("FAIL   : listing does not fit\n");
        return 2;
    }
    std::printf("listing:\n%s", source);

    DWORD raw[max_tokens_] {};
    UINT raw_count = 0;
    if (!assemble(forced ? forced : canonical_assembler_, source, raw, raw_count)) {
        std::printf("FAIL   : the canonical assembler could not assemble the listing\n");
        return 1;
    }

    DWORD tokens[max_tokens_] {};
    UINT stripped = 0;
    const UINT count = strip_leading_comments(raw, raw_count, tokens, stripped);
    if (stripped) {
        std::printf("stripped: %u dwords of leading comment token\n",
            static_cast<unsigned>(stripped));
    }

    // A genuine d3dx8, if this machine has one, has to agree before anything is
    // written. Its absence is the normal case here and is not a failure.
    for (const char* candidate : default_assemblers_) {
        if (!std::strstr(candidate, "d3dx8")) {
            continue;
        }
        DWORD other_raw[max_tokens_] {};
        UINT other_raw_count = 0;
        if (!assemble(candidate, source, other_raw, other_raw_count)) {
            continue;
        }
        DWORD other[max_tokens_] {};
        UINT other_stripped = 0;
        const UINT other_count =
            strip_leading_comments(other_raw, other_raw_count, other, other_stripped);
        if (other_count != count || std::memcmp(other, tokens, count * sizeof(DWORD)) != 0) {
            std::printf("FAIL   : %s disagrees with %s; nothing written\n",
                candidate, canonical_assembler_);
            return 5;
        }
        std::printf("agrees : %s produced the same %u dwords\n", candidate,
            static_cast<unsigned>(count));
    }

    char block[16384] {};
    if (!build_array_source(shader, tokens, count, block, sizeof(block))) {
        return 6;
    }
    std::printf("%s", block);

    if (!rewrite_generated_span(header_path, shader, block)) {
        return 7;
    }
    std::printf("written: %s, %u dwords into %s\n", shader.array_name,
        static_cast<unsigned>(count), header_path);
    return 0;
}


// One shader's whole proof: assemble, strip the banner, compare, print. The
// return is the process exit code, so 0 is the only good answer.
int verify(const ShaderCase& shader, const char* forced, const char* header_path) {
    std::printf("\n==== %s shader ====\n", shader.name);

    char source[4096];
    if (!build_listing(shader, source, sizeof(source))) {
        std::printf("FAIL   : listing does not fit\n");
        return 2;
    }

    std::printf("listing:\n%s", source);

    DWORD raw[max_tokens_] {};
    UINT raw_count = 0;
    bool assembled = false;

    if (forced) {
        assembled = assemble(forced, source, raw, raw_count);
    } else {
        for (const char* candidate : default_assemblers_) {
            assembled = assemble(candidate, source, raw, raw_count);
            if (assembled) {
                break;
            }
        }
    }

    if (!assembled) {
        std::printf("FAIL   : no D3DXAssembleShader could assemble the listing\n");
        return 1;
    }

    DWORD assembled_tokens[max_tokens_] {};
    UINT stripped = 0;
    const UINT assembled_count = strip_leading_comments(raw, raw_count, assembled_tokens, stripped);
    if (stripped) {
        std::printf("stripped: %u dwords of leading comment token\n", static_cast<unsigned>(stripped));
    }

    const DWORD* embedded = shader.tokens;
    const UINT embedded_count = shader.token_count;

    std::printf("embedded : %u dwords, first 0x%08lX, last 0x%08lX\n",
        static_cast<unsigned>(embedded_count),
        static_cast<unsigned long>(embedded[0]),
        static_cast<unsigned long>(embedded[embedded_count - 1]));
    std::printf("assembled: %u dwords, first 0x%08lX, last 0x%08lX\n",
        static_cast<unsigned>(assembled_count),
        static_cast<unsigned long>(assembled_tokens[0]),
        static_cast<unsigned long>(assembled_tokens[assembled_count - 1]));

    const bool same = assembled_count == embedded_count
        && std::memcmp(assembled_tokens, embedded, assembled_count * sizeof(DWORD)) == 0;

    if (!same) {
        std::printf("FAIL   : the embedded tokens are not what the listing assembles to\n");
        dump("embedded", embedded, embedded_count);
        dump("assembled", assembled_tokens, assembled_count);
        return 3;
    }

    std::printf("PASS   : %u dwords byte-identical\n", static_cast<unsigned>(embedded_count));

    // Informational: the declaration is data, not assembly, so nothing can
    // assemble it -- it is printed so a reviewer can read the tokens.
    dump("declaration", shader.declaration, shader.declaration_count);

    if (!listing_matches_header(shader, header_path)) {
        return 4;
    }

    return 0;
}

int main(int argc, char** argv) {
    // [verify|generate] is optional and comes first; without it the arguments
    // mean what they always meant, so `build.sh d3dx8.dll` still forces one.
    int argument = 1;
    bool generating = false;
    if (argc > argument && (std::strcmp(argv[argument], "generate") == 0
            || std::strcmp(argv[argument], "verify") == 0)) {
        generating = std::strcmp(argv[argument], "generate") == 0;
        ++argument;
    }

    // A dash for the assembler means "whichever answers", so a header path can
    // be given without naming one.
    const char* forced = argc > argument ? argv[argument] : nullptr;
    if (forced && std::strcmp(forced, "-") == 0) {
        forced = nullptr;
    }
    const char* header_path = argc > argument + 1 ? argv[argument + 1]
        : "../../ffxi_world_draw.h";

    // Every shader is attempted even after one fails, so a run reports the
    // whole picture rather than the first thing wrong with it.
    int failure = 0;

    // Generation rewrites the header first; the verification below then reads
    // back what it wrote, so a generate run ends in the same proof a plain run
    // gives and never leaves behind a header nobody has checked.
    if (generating) {
        for (int i = 0; i < shader_case_count_; ++i) {
            const int result = generate(shader_cases_[i], forced, header_path);
            if (result != 0 && failure == 0) {
                failure = result;
            }
        }
        if (failure != 0) {
            std::printf("\nFAIL   : generation failed with code %d\n", failure);
            return failure;
        }
        std::printf("\ngenerated: %d token arrays into %s\n",
            shader_case_count_, header_path);
    }
    for (int i = 0; i < shader_case_count_; ++i) {
        const int result = verify(shader_cases_[i], forced, header_path);
        if (result != 0 && failure == 0) {
            failure = result;
        }
    }

    if (failure != 0) {
        std::printf("\nFAIL   : %d shader(s) checked, first failure code %d\n",
            shader_case_count_, failure);
        return failure;
    }

    std::printf("\nPASS   : all %d shaders byte-identical to their listings\n",
        shader_case_count_);
    return 0;
}
