# libwebsockets – Windows pre-built binaries

This directory holds the libwebsockets headers and import libraries needed to
build the RetroArch WebSocket server (`network/ws_server.c`) on Windows with
MinGW-w64 or MSVC.

## Directory layout expected by the build system

```
deps/libwebsockets/
├── include/
│   └── libwebsockets.h      ← and any other lws_*.h headers
├── x64/
│   ├── websockets.lib        ← MSVC import library  (x86-64)
│   └── websockets.dll        ← runtime DLL          (x86-64)
└── arm64/
    ├── websockets.lib        ← MSVC import library  (ARM64)
    └── websockets.dll        ← runtime DLL          (ARM64)
```

For MinGW builds the `.lib` file can be replaced with the equivalent
`libwebsockets.a` (static) or `libwebsockets.dll.a` (import) produced by the
libwebsockets CMake build.

## How to obtain / build libwebsockets

### Option A – vcpkg (recommended for MSVC)

```powershell
# x64
vcpkg install libwebsockets:x64-windows
# ARM64
vcpkg install libwebsockets:arm64-windows
```

Copy the resulting `include/`, `lib/`, and `bin/` artefacts into the layout
above.

### Option B – build from source with CMake

```bat
git clone https://github.com/warmcat/libwebsockets.git
cd libwebsockets
cmake -B build-x64 -A x64  ^
      -DLWS_WITH_SSL=OFF    ^
      -DLWS_WITH_SHARED=ON  ^
      -DCMAKE_INSTALL_PREFIX=install-x64
cmake --build build-x64 --config Release
cmake --install build-x64 --config Release
```

Repeat with `-A ARM64` and adjust the install prefix for ARM64.

## Enabling the feature in the build

### MinGW (Makefile.win)

```makefile
HAVE_WEBSOCKET_SERVER = 1
```

### MSVC (Makefile.msvc)

```makefile
HAVE_WEBSOCKET_SERVER := 1
```

The build system will automatically pick up headers from
`deps/libwebsockets/include` and the correct architecture library from
`deps/libwebsockets/x64` or `deps/libwebsockets/arm64`.

## Runtime

Place `websockets.dll` (from the matching architecture directory) alongside
`retroarch.exe` before launching the application.
