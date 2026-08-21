// gen_slots - the proof behind the vtable indices in daemon/worlddraw_abi.h.
//
// The daemon patches six IDirect3DDevice8 methods by index. An index that is
// one off patches a different method, and forwarding a 7-argument call through
// a 1-argument passthrough corrupts the render thread's stack on every frame --
// v2 of the design had EndScene=36 written by hand, and 36 is Clear. So the
// indices are never typed: this reads the SDK's d3d8.h, counts the STDMETHOD
// entries of the IDirect3DDevice8 DECLARE_INTERFACE_ block in order (that order
// IS the vtable), and either writes them into the header or proves the header
// still agrees.
//
//   build.sh                   verify: exits non-zero if the header has drifted
//   build.sh generate          rewrites the header's generated span, then verifies
//   gen_slots [verify|generate] [d3d8.h] [worlddraw_abi.h]
//
// It also holds the six expected indices and the expected method count, so a
// change to either the SDK or this tool has to be looked at by a person rather
// than being silently generated into the daemon.

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Expectation {
    const char* method;
    const char* constant;
    int index;
};

// What the design measured, and what the daemon is written against.
const Expectation expectations_[] = {
    {"Reset", "WD_SLOT_RESET", 14},
    {"SetRenderTarget", "WD_SLOT_SET_RENDER_TARGET", 31},
    {"DrawPrimitive", "WD_SLOT_DRAW_PRIMITIVE", 70},
    {"DrawIndexedPrimitive", "WD_SLOT_DRAW_INDEXED_PRIMITIVE", 71},
    {"DrawPrimitiveUP", "WD_SLOT_DRAW_PRIMITIVE_UP", 72},
    {"DrawIndexedPrimitiveUP", "WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP", 73},
};
const int expectation_count_ = static_cast<int>(sizeof(expectations_) / sizeof(expectations_[0]));
const int expected_method_count_ = 97;

const int max_methods_ = 256;
const int max_name_ = 64;

char* read_file(const char* path, size_t& length) {
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

    char* text = static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1));
    if (!text) {
        std::fclose(file);
        return nullptr;
    }

    length = std::fread(text, 1, static_cast<size_t>(size), file);
    std::fclose(file);
    text[length] = '\0';
    return text;
}

// The declaration runs from DECLARE_INTERFACE_(IDirect3DDevice8 to the "};"
// that closes it, and every STDMETHOD/STDMETHOD_ inside is one vtable entry, in
// order. Nothing else in the block declares a method.
int collect_methods(const char* text, char names[][max_name_], int capacity) {
    const char* declaration = std::strstr(text, "DECLARE_INTERFACE_(IDirect3DDevice8");
    if (!declaration) {
        std::printf("FAIL   : d3d8.h has no DECLARE_INTERFACE_(IDirect3DDevice8 block\n");
        return -1;
    }

    const char* end = std::strstr(declaration, "\n};");
    if (!end) {
        std::printf("FAIL   : the IDirect3DDevice8 block does not close\n");
        return -1;
    }

    int count = 0;
    for (const char* cursor = declaration; cursor < end;) {
        const char* hit = std::strstr(cursor, "STDMETHOD");
        if (!hit || hit >= end) {
            break;
        }

        const char* open = hit + std::strlen("STDMETHOD");
        if (*open == '_') {
            ++open;
        }
        if (*open != '(') {
            cursor = hit + 1;
            continue;
        }

        const char* close = std::strchr(open, ')');
        if (!close || close >= end) {
            std::printf("FAIL   : an STDMETHOD entry does not close\n");
            return -1;
        }

        // STDMETHOD_(type,Name) carries the return type first; STDMETHOD(Name)
        // does not. Either way the name is the last comma-separated field.
        const char* name = open + 1;
        for (const char* scan = name; scan < close; ++scan) {
            if (*scan == ',') {
                name = scan + 1;
            }
        }
        while (name < close && (*name == ' ' || *name == '\t')) {
            ++name;
        }

        const char* tail = close;
        while (tail > name && (tail[-1] == ' ' || tail[-1] == '\t')) {
            --tail;
        }

        const size_t length = static_cast<size_t>(tail - name);
        if (length == 0 || length + 1 > static_cast<size_t>(max_name_)) {
            std::printf("FAIL   : an STDMETHOD entry has an unusable name\n");
            return -1;
        }
        if (count >= capacity) {
            std::printf("FAIL   : more methods than this tool can hold\n");
            return -1;
        }

        std::memcpy(names[count], name, length);
        names[count][length] = '\0';
        ++count;
        cursor = close + 1;
    }

    return count;
}

int index_of(char names[][max_name_], int count, const char* method) {
    int found = -1;
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(names[i], method) == 0) {
            if (found >= 0) {
                std::printf("FAIL   : %s appears at both %d and %d\n", method, found, i);
                return -2;
            }
            found = i;
        }
    }
    return found;
}

// Exactly the text that lives between the BEGIN and END markers, so verifying
// is a byte comparison of what this would write against what is there.
bool build_block(char names[][max_name_], int count, char* out, size_t size) {
    size_t used = 0;
    const int written = std::snprintf(out, size, "enum {\n");
    if (written < 0) {
        return false;
    }
    used += static_cast<size_t>(written);

    for (int i = 0; i < expectation_count_; ++i) {
        const int index = index_of(names, count, expectations_[i].method);
        if (index < 0) {
            std::printf("FAIL   : d3d8.h has no IDirect3DDevice8::%s\n", expectations_[i].method);
            return false;
        }

        char entry[128];
        const int entry_length = std::snprintf(entry, sizeof(entry), "    %s = %d,",
            expectations_[i].constant, index);
        if (entry_length < 0 || entry_length >= static_cast<int>(sizeof(entry))) {
            return false;
        }

        int padded = entry_length;
        while (padded < 44 && padded + 1 < static_cast<int>(sizeof(entry))) {
            entry[padded] = ' ';
            ++padded;
        }
        entry[padded] = '\0';

        const int line = std::snprintf(out + used, size - used, "%s/* IDirect3DDevice8::%s */\n",
            entry, expectations_[i].method);
        if (line < 0 || static_cast<size_t>(line) >= size - used) {
            return false;
        }
        used += static_cast<size_t>(line);
    }

    const int tail = std::snprintf(out + used, size - used,
        "    WD_SLOT_COUNT = %d,\n"
        "    WD_DEVICE_VTABLE_SLOTS = %d\n"
        "};\n",
        expectation_count_, count);
    return tail > 0 && static_cast<size_t>(tail) < size - used;
}

const char begin_marker_[] = "// >>> BEGIN GENERATED wd_slots";
const char end_marker_[] = "// <<< END GENERATED wd_slots";

// Replaces exactly the enum between the markers. The markers, the comment above
// them and the rest of the header are left alone.
bool rewrite_generated_span(const char* path, const char* block) {
    size_t length = 0;
    char* text = read_file(path, length);
    if (!text) {
        std::printf("FAIL   : %s could not be read\n", path);
        return false;
    }

    char* begin = std::strstr(text, begin_marker_);
    char* end = begin ? std::strstr(begin, end_marker_) : nullptr;
    char* span = begin ? std::strstr(begin, "enum {") : nullptr;
    if (!begin || !end || !span || span > end) {
        std::printf("FAIL   : %s has no enum between BEGIN/END GENERATED wd_slots\n", path);
        std::free(text);
        return false;
    }

    FILE* file = std::fopen(path, "wb");
    if (!file) {
        std::printf("FAIL   : %s could not be written\n", path);
        std::free(text);
        return false;
    }
    std::fwrite(text, 1, static_cast<size_t>(span - text), file);
    std::fwrite(block, 1, std::strlen(block), file);
    std::fwrite(end, 1, length - static_cast<size_t>(end - text), file);
    std::fclose(file);
    std::free(text);
    return true;
}

bool verify_generated_span(const char* path, const char* block) {
    size_t length = 0;
    char* text = read_file(path, length);
    if (!text) {
        std::printf("FAIL   : %s could not be read\n", path);
        return false;
    }

    const char* begin = std::strstr(text, begin_marker_);
    const char* end = begin ? std::strstr(begin, end_marker_) : nullptr;
    const char* span = begin ? std::strstr(begin, "enum {") : nullptr;
    bool ok = false;
    if (!begin || !end || !span || span > end) {
        std::printf("FAIL   : %s has no enum between BEGIN/END GENERATED wd_slots\n", path);
    } else {
        const size_t present = static_cast<size_t>(end - span);
        ok = present == std::strlen(block) && std::memcmp(span, block, present) == 0;
        if (!ok) {
            std::printf("FAIL   : %s has drifted from what d3d8.h says.\n", path);
            std::printf("---- header ----\n%.*s---- d3d8.h ----\n%s----\n",
                static_cast<int>(present), span, block);
        }
    }

    std::free(text);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    int argument = 1;
    bool generating = false;
    if (argc > argument && (std::strcmp(argv[argument], "generate") == 0
            || std::strcmp(argv[argument], "verify") == 0)) {
        generating = std::strcmp(argv[argument], "generate") == 0;
        ++argument;
    }

    const char* sdk_path = argc > argument ? argv[argument]
        : "/usr/i686-w64-mingw32/include/d3d8.h";
    const char* header_path = argc > argument + 1 ? argv[argument + 1]
        : "../../daemon/worlddraw_abi.h";

    size_t length = 0;
    char* sdk = read_file(sdk_path, length);
    if (!sdk) {
        std::printf("FAIL   : %s could not be read\n", sdk_path);
        return 2;
    }

    static char names[max_methods_][max_name_];
    const int count = collect_methods(sdk, names, max_methods_);
    std::free(sdk);
    if (count < 0) {
        return 3;
    }

    std::printf("sdk    : %s\n", sdk_path);
    std::printf("methods: %d in IDirect3DDevice8\n", count);
    if (count != expected_method_count_) {
        std::printf("FAIL   : expected %d methods, the SDK declares %d\n",
            expected_method_count_, count);
        for (int i = 0; i < count; ++i) {
            std::printf("         %3d %s\n", i, names[i]);
        }
        return 4;
    }

    int failure = 0;
    for (int i = 0; i < expectation_count_; ++i) {
        const int index = index_of(names, count, expectations_[i].method);
        if (index != expectations_[i].index) {
            std::printf("FAIL   : %s is %d in the SDK, the design measured %d\n",
                expectations_[i].method, index, expectations_[i].index);
            failure = 5;
            continue;
        }
        std::printf("slot   : %-24s %3d  %s\n", expectations_[i].method, index,
            expectations_[i].constant);
    }
    if (failure != 0) {
        return failure;
    }

    // Neighbours of every hooked slot, so a one-off shows up as a name a reader
    // recognises rather than as a number.
    for (int i = 0; i < expectation_count_; ++i) {
        const int index = expectations_[i].index;
        std::printf("around : %d %s | %d %s | %d %s\n",
            index - 1, index > 0 ? names[index - 1] : "-",
            index, names[index],
            index + 1, index + 1 < count ? names[index + 1] : "-");
    }

    char block[2048];
    if (!build_block(names, count, block, sizeof(block))) {
        std::printf("FAIL   : the generated enum does not fit\n");
        return 6;
    }

    if (generating) {
        if (!rewrite_generated_span(header_path, block)) {
            return 7;
        }
        std::printf("written: %d indices into %s\n", expectation_count_, header_path);
        std::printf("%s", block);
    }

    if (!verify_generated_span(header_path, block)) {
        return 8;
    }

    std::printf("PASS   : %s carries the %d indices d3d8.h declares\n",
        header_path, expectation_count_);
    return 0;
}
