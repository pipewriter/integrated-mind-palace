# Building Exodia MP on Windows (MSYS2/MinGW)

## 1. Install MSYS2

Download and install from https://www.msys2.org/

After install, open **MSYS2 UCRT64** (not MSYS2 MSYS — use the UCRT64 terminal).

## 2. Install dependencies

```bash
pacman -Syu
pacman -S \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-make \
  mingw-w64-ucrt-x86_64-glfw \
  mingw-w64-ucrt-x86_64-vulkan-loader \
  mingw-w64-ucrt-x86_64-vulkan-headers \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-SDL2 \
  mingw-w64-ucrt-x86_64-zlib \
  git
```

## 3. Install Vulkan SDK (for glslc shader compiler)

Download from https://vulkan.lunarg.com/sdk/home#windows and install. The MSYS2 Vulkan loader handles linking, but glslc is needed at runtime for shader compilation.

Add glslc to PATH if not already — typically `C:\VulkanSDK\<version>\Bin`.

## 4. Clone and build

```bash
cd /c/Users/$USER/Desktop  # or wherever
git clone <repo-url> exodia
cd exodia
```

Build with the Windows Makefile:

```bash
mingw32-make -f Makefile.win
```

This produces `client.exe` and `server.exe`.

## 5. Connect to a server

```bash
./client.exe --host 54.87.17.228 --port 10001
```

Or run a local server:

```bash
./server.exe --port 9998 --seed 12345
./client.exe --host 127.0.0.1 --port 9998
```

## 6. GPU drivers

Make sure you have up-to-date GPU drivers with Vulkan support:
- NVIDIA: https://www.nvidia.com/drivers
- AMD: https://www.amd.com/en/support
- Intel: https://www.intel.com/content/www/us/en/download-center

## Troubleshooting

- **"glslc not found"**: Install the Vulkan SDK and ensure glslc is on PATH
- **Black screen**: Update GPU drivers, ensure Vulkan is supported
- **Link errors**: Make sure you're using the UCRT64 terminal, not MSYS2 MSYS
- **Missing DLLs at runtime**: Run from the MSYS2 UCRT64 terminal, or copy the required DLLs next to client.exe
