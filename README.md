# Easy-GPU-Tools

Cross-platform GPU information tool used by [llama.cpp-hub](https://github.com/IIIIIllllIIIIIlllll/llama.cpp-hub).

**Features**
- Enumerates all GPUs via Vulkan (NVIDIA / AMD / Intel)
- NVIDIA dynamic info: temperature, utilization, clocks, power, memory, fan, ECC (NVML)
- AMD Linux dynamic info: temperature, utilization, clocks, power (sysfs)
- AMD Windows dynamic info: temperature, fan, clocks, utilization (ADL)
- Zero runtime dependencies — all vendor libraries loaded dynamically

## Build

### Windows

```powershell
cmake -B build -S .
cmake --build build --config Release
.\build\Release\gpu-info.exe
```

### Linux

```bash
sudo apt install gcc cmake libvulkan-dev mesa-vulkan-drivers

cmake -B build -S .
cmake --build build
./build/gpu-info
```

### Output example

```
===== GPU Information (Vulkan + ADL/sysfs + NVML) =====
=======================================================

Device 0: Tesla V100-SXM2-16GB
  Vendor          : NVIDIA (0x10DE)
  Type            : Discrete GPU
  Dedicated VRAM  : 16.00 GB
  Vulkan API      : 1.4.312

  --- NVIDIA Dynamic Info (NVML) ---
  GPU Temperature : 31 C
  GPU Utilization : 0%
  SM Clock        : 135 MHz
  Memory Usage    : 0 / 16384 MB
  Power           : 21 W / 300 W
  ECC             : Enabled

Device 1: AMD Radeon Graphics (RADV GFX1151)
  Vendor          : AMD (0x1002)
  Type            : Integrated GPU

  --- AMD Dynamic Info (sysfs) ---
  GPU Temperature : 45.0 C
  GPU Utilization : 15%
  Engine Clock    : 2500 MHz
  Power           : 120.02 W
---
Total: 2 GPU(s) detected.
```
