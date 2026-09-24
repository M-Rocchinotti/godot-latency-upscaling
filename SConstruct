#!/usr/bin/env python
"""Build the godot-latency-upscaling GDExtension and its C++ unit tests.

Normally run through `build.cmd` / `python tools/build.py`, which prepares the Python venv
with a pinned SCons and the MSVC environment. Direct use from a developer prompt:

    scons target=template_debug     # addons/latency_upscaling/bin/...template_debug.x86_64.dll
    scons target=template_release   # addons/latency_upscaling/bin/...template_release.x86_64.dll
    scons unit_tests                # build/unit_tests/unit_tests.exe (no Godot needed)
"""
import os

# A developer shell (vcvars, or a portable-MSVC setup script) exports INCLUDE and LIB.
# SCons' own autodetection only finds registered Visual Studio installs, so hand the shell's
# settings to it explicitly when they are present.
msvc_settings = None
if os.environ.get("INCLUDE") and os.environ.get("LIB"):
    msvc_settings = {k: os.environ[k] for k in ("PATH", "INCLUDE", "LIB", "LIBPATH") if os.environ.get(k)}

if msvc_settings:
    base_env = Environment(tools=["default"], PLATFORM="", MSVC_USE_SETTINGS=msvc_settings)
else:
    base_env = Environment(tools=["default"], PLATFORM="")

# Only generate godot-cpp bindings for the engine classes this extension uses.
ARGUMENTS.setdefault("build_profile", File("tools/godot_cpp_build_profile.json").abspath)

env = SConscript("thirdparty/godot-cpp/SConstruct", {"env": base_env, "api_version": "4.7"})

if env["platform"] != "windows" or env["arch"] != "x86_64":
    print("godot-latency-upscaling targets Windows x86_64 only (the Vulkan hook is Win32-specific).")
    Exit(1)

godot_cpp_env = env
env = godot_cpp_env.Clone()  # our flags must not change how godot-cpp itself is compiled
env.Append(CPPPATH=["src/", "thirdparty/Vulkan-Headers/include/"])
# All Vulkan entry points are resolved at run time through the hooked loader; the extension
# must never link against (and so implicitly load) vulkan-1.dll itself.
env.Append(CPPDEFINES=["VK_NO_PROTOTYPES", "WIN32_LEAN_AND_MEAN"])
# /W4 for our code; headers included with <...> (godot-cpp, Vulkan) are third-party and silent.
env.Append(CXXFLAGS=["/std:c++17", "/W4", "/permissive-", "/external:anglebrackets", "/external:W0"])
env.Append(LIBS=["kernel32", "advapi32"])

# Modules without any Godot dependency. They are compiled into the extension AND into the
# unit-test executable, so their logic is tested without starting the engine.
PURE_SOURCES = [
    "src/core/log.cpp",
    "src/core/settings.cpp",
    "src/dlss/jitter.cpp",
    "src/hook/iat_patch.cpp",
    "src/hook/loader_name.cpp",
    "src/reflex/frame_sequencer.cpp",
    "src/reflex/latency_stats.cpp",
    "src/reflex/ll2_patches.cpp",
    "src/vk/device_patch.cpp",
]

obj_dir = "build/obj/" + env["target"]
env.VariantDir(obj_dir, "src", duplicate=False)
sources = [s.replace("src/", obj_dir + "/", 1) for s in PURE_SOURCES]
for pattern in ("*.cpp", "*/*.cpp"):
    for node in Glob("src/" + pattern):
        rel = node.path.replace("\\", "/")
        if rel not in PURE_SOURCES:
            sources.append(rel.replace("src/", obj_dir + "/", 1))

# The DLL exports its entry point and vkGetInstanceProcAddr but nothing links against it, so the
# MSVC import library and export file are build artefacts, not part of the addon: keep them out of
# addons/.../bin (which is packaged) by emitting them under build/.
lib_env = env.Clone()
lib_env["no_import_lib"] = 1
lib_env.Append(LINKFLAGS=["/IMPLIB:build/obj/{}/godot_latency_upscaling{}.lib".format(env["target"], env["suffix"])])
library = lib_env.SharedLibrary(
    "addons/latency_upscaling/bin/godot_latency_upscaling{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=sources,
)
Default(library)

# C++ unit tests: the pure modules plus a tiny test harness, linked into a console program.
test_env = env.Clone()
test_env["LIBS"] = ["kernel32", "advapi32"]
test_env.Append(CPPDEFINES=["LU_UNIT_TESTS"])
test_obj_dir = "build/obj/unit_tests"
test_env.VariantDir(test_obj_dir + "/src", "src", duplicate=False)
test_env.VariantDir(test_obj_dir + "/tests", "tests/unit", duplicate=False)
test_sources = [s.replace("src/", test_obj_dir + "/src/", 1) for s in PURE_SOURCES]
test_sources += [n.path.replace("\\", "/").replace("tests/unit/", test_obj_dir + "/tests/", 1)
                 for n in Glob("tests/unit/*.cpp")]
unit_tests = test_env.Program("build/unit_tests/unit_tests.exe", test_sources)
Alias("unit_tests", unit_tests)
