#!/usr/bin/env python3
"""gate_device_calls - fails the build if a device method is REACHABLE from a
Lua entry point.

THE RULE IT ENFORCES. FFXI's D3D8 device is created without
D3DCREATE_MULTITHREADED and its hooks run on a different OS thread from
Windower's Lua, so the runtime takes no lock of its own and a device method
called from an l_* entry point is an access violation inside d3d8.dll on a real
client. A lock of ours cannot fix it: the GAME's render thread calls the same
device and cannot be made to take one. So the rule is absolute, and this is the
static half of checking it -- worlddraw_harness.cpp's thread gate is the
behavioural half, and neither replaces the other.

HOW. It reads the preprocessed text the harness build already produces
(build/worlddraw.i), keeps only the regions that came from this library's own
two files, finds every function DEFINED in them, builds the call graph over
those definitions, and walks it from every l_* entry point. Any function
reachable that way whose body contains a call through a Direct3D interface --
`something->CreateVertexBuffer(`, `->Release()`, and the rest of the list below
-- fails the build and is printed with the path that reaches it.

WHAT IT CANNOT SEE, said plainly so nobody reads more into a pass than is
there:

  - a call through a function POINTER. The daemon is reached that way
    (`daemon_api_->ensure_hooks(device)`), and what the daemon does with the
    device is the daemon's own business and its own harness's.
  - anything outside ffxi_world_draw.h and lua/worlddraw.cpp.

The call graph is deliberately OVER-approximate: an identifier followed by `(`
inside a body is taken as a call to a function of that name if one is defined
in these files. That errs towards flagging, which is the direction a gate
should err in.

    python3 gate_device_calls.py build/worlddraw.i     # non-zero if it fails
"""

import re
import sys

# Methods of IDirect3DDevice8 and of the interfaces it hands out. Spelled as
# `->NAME(` so a call THROUGH a pointer is what matches: a plain `Release(` of
# our own would not, and neither would a comment.
DEVICE_METHODS = [
    "AddRef", "BeginScene", "CreateIndexBuffer", "CreateTexture",
    "CreateVertexBuffer", "CreateVertexShader", "DeleteVertexShader",
    "DrawIndexedPrimitive", "DrawIndexedPrimitiveUP", "DrawPrimitive",
    "DrawPrimitiveUP", "EndScene", "GetCreationParameters",
    "GetDepthStencilSurface", "GetRenderState", "GetRenderTarget",
    "GetStreamSource", "GetTexture", "GetTextureStageState", "GetTransform",
    "GetVertexShader", "GetVertexShaderConstant", "GetViewport", "Lock",
    "LockRect", "Present", "Release", "Reset", "SetRenderState",
    "SetRenderTarget", "SetStreamSource", "SetTexture", "SetTextureStageState",
    "SetTransform", "SetVertexShader", "SetVertexShaderConstant", "SetViewport",
    "TestCooperativeLevel", "Unlock", "UnlockRect",
]

# GDI+ teardown is on the same list for the same reason: it belongs to the
# queue the render thread drains, not to a handle closing on the Lua thread.
BARE_CALLS = ["GdiplusShutdown"]

DEVICE_CALL = re.compile(
    r"->\s*(" + "|".join(DEVICE_METHODS) + r")\s*\("
    + r"|\b(" + "|".join(BARE_CALLS) + r")\s*\("
)

OUR_FILES = ("ffxi_world_draw.h", "worlddraw.cpp")

# `if (...) {` is not a function definition. Neither is any other statement
# that puts a parenthesised thing in front of a brace.
KEYWORDS = {
    "if", "for", "while", "switch", "catch", "do", "else", "return", "sizeof",
    "alignof", "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast",
    "and", "or", "not", "new", "delete", "throw", "case", "default",
}

LINE_MARKER = re.compile(r'^#\s+\d+\s+"([^"]*)"')
# The tail of a function signature: ...NAME ( args ) [const] [override] ... {
SIGNATURE = re.compile(
    r"(?:^|[\s*&:~])([A-Za-z_]\w*)\s*\([^;{}]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?$"
)
IDENTIFIER_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def keep_our_regions(text):
    """The preprocessed text with everything that came from somebody else's
    header dropped, and the line markers with it."""
    kept = []
    current = ""
    for line in text.splitlines():
        marker = LINE_MARKER.match(line)
        if marker:
            current = marker.group(1).replace("\\", "/").rsplit("/", 1)[-1]
            kept.append("")
            continue
        kept.append(line if current in OUR_FILES else "")
    return "\n".join(kept)


def find_functions(text):
    """Every function defined in `text`, as name -> body. Brace-matched, so a
    body is exactly what the function contains and nothing after it."""
    functions = {}
    i = 0
    length = len(text)
    while i < length:
        character = text[i]
        if character != "{":
            i += 1
            continue

        # What comes before this brace decides whether it opens a function.
        head_start = max(
            text.rfind(";", 0, i), text.rfind("{", 0, i), text.rfind("}", 0, i)
        ) + 1
        head = text[head_start:i].strip()
        match = SIGNATURE.search(head)
        if not match or match.group(1) in KEYWORDS:
            i += 1
            continue

        depth = 0
        j = i
        while j < length:
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if depth != 0:
            i += 1
            continue

        name = match.group(1)
        body = text[i:j + 1]
        # Overloads and same-named members of different classes are merged:
        # every one of them is then treated as reachable from every caller,
        # which is the conservative direction.
        functions[name] = functions.get(name, "") + body
        i = j + 1
    return functions


def main(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        text = keep_our_regions(handle.read())

    functions = find_functions(text)
    if len(functions) < 50:
        print("CHECK FAILED: the device-call gate parsed only %d functions out of %s;"
              % (len(functions), path))
        print("              it is not looking at what it thinks it is looking at")
        return 1

    roots = sorted(name for name in functions if name.startswith("l_"))
    if len(roots) < 20:
        print("CHECK FAILED: the device-call gate found only %d l_* entry points"
              % len(roots))
        return 1

    calls = {}
    device_calls = {}
    for name, body in functions.items():
        calls[name] = {
            called for called in IDENTIFIER_CALL.findall(body)
            if called in functions and called != name
        }
        found = sorted({(a or b) for a, b in DEVICE_CALL.findall(body)})
        if found:
            device_calls[name] = found

    failures = []
    for root in roots:
        seen = {root}
        stack = [(root, [root])]
        while stack:
            name, path_to = stack.pop()
            if name in device_calls:
                failures.append((root, path_to, device_calls[name]))
                continue
            for called in sorted(calls.get(name, ())):
                if called not in seen:
                    seen.add(called)
                    stack.append((called, path_to + [called]))

    if failures:
        print("CHECK FAILED: a device method is reachable from a Lua entry point")
        for root, path_to, methods in failures:
            print("              %s: %s" % (root, " -> ".join(path_to)))
            print("                calls %s" % ", ".join(methods))
        return 1

    print("device : no device method is reachable from any of the %d l_* entry"
          % len(roots))
    print("         points, over %d functions of ours" % len(functions))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/worlddraw.i"))
