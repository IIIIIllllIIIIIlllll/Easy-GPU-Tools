# Easy-GPU-Tools

Cross-platform GPU information tool used by [llama.cpp-hub](https://github.com/IIIIIllllIIIIIlllll/llama.cpp-hub).

**Features**
- GPU enumeration via Vulkan (NVIDIA / AMD / Intel) with dynamic sensor data
  - NVIDIA: temperature, utilization, clocks, power, memory, fan, ECC (NVML)
  - AMD Linux: temperature, utilization, clocks, power (sysfs)
  - AMD Windows: temperature, fan, clocks, utilization (ADL)
- System info via `--sysinfo`: OS, CPU, RAM, disk
- Zero runtime dependencies — all vendor libraries loaded dynamically
- Structured JSON output for GPU (`--json`) and system (`--sysinfo --json`)
- Combined output mode (`--all`) for GPU + system info in a single call

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

## Usage

```
gpu-info                  # GPU info + system info, human-readable text (default)
gpu-info --json           # GPU info + system info, machine-readable JSON
gpu-info --gpu            # GPU info only (skip system info)
gpu-info --gpu --json     # GPU info only, JSON
gpu-info --sysinfo        # System info (OS/CPU/RAM/Disk), human-readable text
gpu-info --sysinfo --json # System info, JSON
gpu-info --all            # GPU + system info, text
gpu-info --all --json     # GPU + system info, single merged JSON
```

## Output format

### Text output (default)

Every GPU prints a block of static Vulkan information, followed by vendor-specific dynamic
sensor data (if the corresponding backend loaded successfully).

```
===== GPU Information (Vulkan + ADL/sysfs + NVML) =====
=======================================================

Device 0: Tesla V100-SXM2-16GB
  Vendor          : NVIDIA (0x10DE)
  Device ID       : 0x1DB5
  Type            : Discrete GPU
  Dedicated VRAM  : 16.00 GB
  Shared RAM      : 8.00 GB
  Vulkan API      : 1.4.312
  Driver Version  : 570.11.0.0 (raw: 0x85C60F0)
  Memory Heaps    : 2
    Heap 0: 8.00 GB     [Host     (System RAM)]
    Heap 1: 16.00 GB    [Device Local (VRAM)]

  --- NVIDIA Dynamic Info (NVML) ---
  GPU Temperature : 31 C
  GPU Utilization : 0%
  Memory Util.    : 0%
  SM Clock        : 135 MHz
  Memory Clock    : 877 MHz
  Memory Usage    : 0 / 16384 MB
  Power           : 21 W / 300 W
  Fan Speed       : 30%
  Perf State      : P8
  ECC             : Enabled
  Driver Version  : 570.11.0
---
Total: 1 GPU(s) detected.
```

**Static fields** (always present, from Vulkan):

| Field | Source |
|---|---|
| Device name | `VkPhysicalDeviceProperties.deviceName` |
| Vendor ID | `props.vendorID` (0x10DE=NVIDIA, 0x1002=AMD, 0x8086=Intel, etc.) |
| Device ID | `props.deviceID` |
| Type | `props.deviceType` (Integrated / Discrete / Virtual / CPU) |
| Dedicated VRAM | Sum of `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT` heaps |
| Shared RAM | Sum of non-device-local heaps |
| Vulkan API | `props.apiVersion` |
| Driver Version | `props.driverVersion` (vendor-specific encoding) |

**Dynamic sensor fields** vary by backend:

| Field | NVML (NVIDIA) | ADL (AMD/Win) | sysfs (AMD/Linux) |
|---|---|---|---|
| Temperature | | | |
| GPU utilization | | | |
| Memory utilization | | — | — |
| Core (SM/Engine) clock | | | |
| Memory clock | | | |
| Power usage | | — | |
| Power limit | | — | — |
| Fan speed (%) | | | |
| Fan speed (RPM) | — | | |
| Performance state | | | — |
| ECC status | | — | — |
| Memory used/total | | — | — |
| Driver version string | | — | — |

Fields marked `—` are always `N/A` for that backend.

### JSON output (`--json`)

Adding `--json` prints a single JSON object to stdout. **Every GPU device has the exact
same set of keys**, regardless of vendor or backend. Unavailable values are `null`.

```json
{
  "devices": [
    {
      "name":                 "Tesla V100-SXM2-16GB",
      "vendor":               "NVIDIA",
      "vendor_id":            4318,
      "device_id":            9916,
      "type":                 "Discrete GPU",

      "memory": {
        "dedicated_vram_bytes": 17179869184,
        "shared_ram_bytes":     8589934592,
        "heaps": [
          {"size_bytes": 8589934592,  "flags": "host"},
          {"size_bytes": 17179869184, "flags": "device_local"}
        ]
      },

      "vulkan": {
        "api_version":       "1.4.312",
        "driver_version":    "570.11.0.0",
        "driver_version_raw":"0x85C60F0"
      },

      "sensors": {
        "backend":               "NVML",
        "temperature_celsius":   31.0,
        "utilization_gpu_pct":   0,
        "utilization_memory_pct":0,
        "core_clock_mhz":        135,
        "memory_clock_mhz":      877,
        "power_watts":           21.0,
        "power_limit_watts":     300.0,
        "fan_speed_rpm":         null,
        "fan_speed_pct":         30,
        "perf_state":            "P8",
        "ecc":                   "enabled",
        "memory_used_bytes":     0,
        "memory_total_bytes":    17179869184,
        "driver_version_str":    "570.11.0"
      }
    }
  ],
  "device_count": 1
}
```

#### JSON field reference

**Top-level**

| Key | Type | Description |
|---|---|---|
| `devices` | array | List of GPU devices |
| `device_count` | int | Number of devices (redundant with array length) |

**Device object** (`devices[n]`)

| Key | Type | Description |
|---|---|---|
| `name` | string | GPU name from Vulkan |
| `vendor` | string | Human-readable vendor name |
| `vendor_id` | int | PCI vendor ID (e.g. 4318 = 0x10DE = NVIDIA) |
| `device_id` | int | PCI device ID |
| `type` | string | `Integrated GPU`, `Discrete GPU`, `Virtual GPU`, or `CPU` |
| `memory` | object | Memory breakdown (see below) |
| `vulkan` | object | Vulkan version info (see below) |
| `sensors` | object | Dynamic sensor data (see below) |

**`memory` object**

| Key | Type | Unit | Description |
|---|---|---|---|
| `dedicated_vram_bytes` | int | bytes | Total device-local video memory |
| `shared_ram_bytes` | int | bytes | Total host-visible system memory |
| `heaps` | array | — | Raw Vulkan memory heaps |
| `heaps[n].size_bytes` | int | bytes | Heap size |
| `heaps[n].flags` | string | — | `device_local` or `host` |

**`vulkan` object**

| Key | Type | Description |
|---|---|---|
| `api_version` | string | Vulkan API version, e.g. `"1.4.312"` |
| `driver_version` | string | Driver version (vendor-encoding decoded) |
| `driver_version_raw` | string | Raw driver version as hex, e.g. `"0x85C60F0"` |

**`sensors` object** — all values are `null` when unavailable.

| Key | Type | Unit | Provided by |
|---|---|---|---|
| `backend` | string | — | Backend name: `NVML`, `ADL`, `sysfs`, or `""` |
| `temperature_celsius` | float | | NVML, ADL, sysfs |
| `utilization_gpu_pct` | int | % | NVML, ADL, sysfs |
| `utilization_memory_pct` | int | % | NVML only |
| `core_clock_mhz` | int | MHz | NVML, ADL, sysfs |
| `memory_clock_mhz` | int | MHz | NVML, ADL, sysfs |
| `power_watts` | float | W | NVML, sysfs |
| `power_limit_watts` | float | W | NVML only |
| `fan_speed_rpm` | int | RPM | ADL, sysfs |
| `fan_speed_pct` | int | % | NVML, ADL |
| `perf_state` | string | — | NVML, ADL (e.g. `"P0"`..`"P15"`) |
| `ecc` | string | — | NVML only (`"enabled"`, `"disabled"`) |
| `memory_used_bytes` | int | bytes | NVML only |
| `memory_total_bytes` | int | bytes | NVML only |
| `driver_version_str` | string | — | NVML driver version string |

#### Scripting examples

```bash
# GPU name
./build/gpu-info --json | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['devices'][0]['name'])"

# Temperature
./build/gpu-info --json | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['devices'][0]['sensors']['temperature_celsius'])"

# VRAM usage (NVML only)
./build/gpu-info --json | python3 -c "
import json, sys
d = json.load(sys.stdin)
for g in d['devices']:
    s = g['sensors']
    if s['memory_total_bytes']:
        used = s['memory_used_bytes'] / (1024**3)
        total = s['memory_total_bytes'] / (1024**3)
        print(f\"{g['name']}: {used:.1f} / {total:.1f} GB\")
    else:
        print(f\"{g['name']}: VRAM usage not available\")
"
```

## System info (`--sysinfo` / `--all`)

Gathers OS, CPU, RAM, and disk information from the host machine.

### Text output

```
===== System Information =====

OS
  Name      : Windows 10 Enterprise LTSC 2021
  Version   : 21H2
  Arch      : x86_64
  Hostname  : DESKTOP-UO6HL14
  Kernel    : WIN32_NT 10.0.19044
  Uptime    : 15 days 2 hours 18 mins

CPU
  Model     : AMD Ryzen 7 8745HS w/ Radeon 780M Graphics
  Cores     : 8 physical / 16 logical
  Max Freq  : 3793 MHz

Memory
  Physical  : 16.91 GB / 23.29 GB (72%)
  Swap      : 6.52 GB / 6.52 GB (100%)

Disk Drives
  C:\        316.69 GB / 952.66 GB (33%)  NTFS
```

### JSON structure

```json
{
  "system": {
    "os": {
      "name": "Windows 10 Enterprise LTSC 2021",
      "version": "21H2",
      "arch": "x86_64",
      "hostname": "DESKTOP-UO6HL14",
      "kernel": "WIN32_NT 10.0.19044",
      "uptime_seconds": 1303200
    },
    "cpu": {
      "name": "AMD Ryzen 7 8745HS w/ Radeon 780M Graphics",
      "cores": 8,
      "threads": 16,
      "freq_max_mhz": 3793
    },
    "memory": {
      "total_bytes": 25006493696,
      "used_bytes": 18159714304,
      "swap_total_bytes": 6996287488,
      "swap_used_bytes": 2662440960
    },
    "disks": [
      {
        "name": "C:",
        "mount": "C:\\",
        "filesystem": "NTFS",
        "total_bytes": 1022912815104,
        "used_bytes": 340044746752,
        "is_external": false
      }
    ]
  }
}
```

#### System JSON field reference

**`system.os`**

| Key | Type | Source |
|---|---|---|
| `name` | string | `/etc/os-release` or Registry `ProductName` |
| `version` | string | `/etc/os-release` or Registry `DisplayVersion` |
| `arch` | string | `uname -m` or `GetNativeSystemInfo` |
| `hostname` | string | `gethostname` / `GetComputerName` |
| `kernel` | string | `uname -sr` / `RtlGetVersion` |
| `uptime_seconds` | int | `/proc/uptime` / `GetTickCount64` |

**`system.cpu`**

| Key | Type | Source |
|---|---|---|
| `name` | string | `/proc/cpuinfo` or Registry `ProcessorNameString` |
| `cores` | int | Physical cores |
| `threads` | int | Logical processors |
| `freq_max_mhz` | int | `/proc/cpuinfo` `cpu MHz` or Registry `~MHz` |

**`system.memory`**

| Key | Type | Unit | Source |
|---|---|---|---|
| `total_bytes` | int | bytes | `MemTotal` / `GlobalMemoryStatusEx` |
| `used_bytes` | int | bytes | `MemTotal - MemAvailable` / `TotalPhys - AvailPhys` |
| `swap_total_bytes` | int | bytes | `SwapTotal` / pagefile size |
| `swap_used_bytes` | int | bytes | `SwapTotal - SwapFree` / paged-out estimate |

**`system.disks[n]`**

| Key | Type | Description |
|---|---|---|
| `name` | string | Volume label (`C:`) or device name (`nvme0n1p2`) |
| `mount` | string | Mount point |
| `filesystem` | string | NTFS, ext4, xfs, btrfs, etc. |
| `total_bytes` | int | Partition total size |
| `used_bytes` | int | Used space |
| `is_external` | bool | `true` for removable drives (Windows only) |

On **Windows**, only fixed drives (`DRIVE_FIXED`) are listed.
On **Linux**, virtual filesystems (proc, sysfs, tmpfs, etc.) are filtered out.

### Scripting examples

```bash
# System uptime in days
./build/gpu-info --sysinfo --json | python3 -c "
import json, sys
s = json.load(sys.stdin)['system']
print(s['os']['uptime_seconds'] / 86400)
"

# Memory usage percentage
./build/gpu-info --sysinfo --json | python3 -c "
import json, sys
m = json.load(sys.stdin)['system']['memory']
print(f\"{m['used_bytes'] / m['total_bytes'] * 100:.1f}%\")
"

# Disk free space
./build/gpu-info --sysinfo --json | python3 -c "
import json, sys
for d in json.load(sys.stdin)['system']['disks']:
    free = (d['total_bytes'] - d['used_bytes']) / (1024**3)
    print(f\"{d['name']}: {free:.1f} GB free\")
"
```
