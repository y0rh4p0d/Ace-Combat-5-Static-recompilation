# Runtime DLLs

`ac5.exe` links against three DLLs at run time:

| DLL | Where it comes from |
| --- | --- |
| `vulkan-1.dll` | your graphics driver, so it is already installed |
| `SDL3.dll` | MSYS2 |
| `libwinpthread-1.dll` | MSYS2 |

Without the last two, starting `ac5.exe` from Explorer fails with a message about
a missing DLL, and it only works from a shell that has MSYS2's `bin` on `PATH`.
The CMake build copies these two next to the executable by default
(`-DPS2_COPY_RUNTIME_DLLS=OFF` disables that), and they are committed here so a
build works without the MSYS2 runtime being on `PATH` at all.

- `x64/` — from `C:\msys64\ucrt64\bin`, which is what the x64 GCC build uses
- `arm64/` — from `C:\msys64\clangarm64\bin`, matching the AArch64 cross build

Each directory holds the DLL for its own architecture; they are not
interchangeable. `LIB-`/`SDL3.dll` mismatches show up as `0xC000007B` ("not a
valid application for this OS platform") rather than a missing-file error.

## Licences

Both are permissively licensed and redistributable; the licence texts are kept
here as `LICENSE-libwinpthread.txt` (MIT and BSD-3-Clause-Clear, mingw-w64
project) and `LICENSE-SDL3.txt` (Zlib, Sam Lantinga).

Nothing else from MSYS2 is included. In particular no GCC runtime is needed:
the build links `-static-libgcc -static-libstdc++`, so there is no
`libstdc++-6.dll` or `libgcc_s_seh-1.dll` dependency.

## Refreshing them

After upgrading MSYS2, copy the newer files across:

```powershell
Copy-Item C:\msys64\ucrt64\bin\SDL3.dll, C:\msys64\ucrt64\bin\libwinpthread-1.dll runtime\dll\x64\
Copy-Item C:\msys64\clangarm64\bin\SDL3.dll, C:\msys64\clangarm64\bin\libwinpthread-1.dll runtime\dll\arm64\
```

`tools/setup_arm64_toolchain.py` installs the matching `arm64` copies from the
MSYS2 packages it downloads.
