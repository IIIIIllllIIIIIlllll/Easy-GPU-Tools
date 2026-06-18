# gpu-info

Cross-platform GPU and system information tool, written in pure C (C11).

## Build

### Windows

```powershell
cmake -B build -S .
cmake --build build --config Release
.\build\Release\gpu-info.exe
```

### Linux

```bash
sudo apt install gcc cmake libvulkan-dev

cmake -B build -S .
cmake --build build
./build/gpu-info
```

## Usage

```
gpu-info               # GPU + system info, text (default)
gpu-info --json        # JSON output
gpu-info --gpu         # GPU info only
gpu-info --sysinfo     # System info only (OS/CPU/RAM/Disk)
gpu-info --all         # GPU + system info
gpu-info --memory      # Memory only (system RAM + GPU VRAM)
gpu-info --cpu         # CPU info only
gpu-info --ram         # RAM info only
```

`--json` can be combined with any flag above to switch to JSON output.
