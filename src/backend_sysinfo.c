#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_sysinfo.h"

/* ================================================================
 *  Windows implementation
 * ================================================================ */
#ifdef _WIN32
#include <windows.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0x00000000L
#endif

typedef LONG NTSTATUS;

typedef struct _OSVERSIONINFOEXW_NT {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} OSVERSIONINFOEXW_NT;

typedef NTSTATUS (WINAPI *RtlGetVersion_t)(OSVERSIONINFOEXW_NT *);

int sys_collect_os_info(SysInfo *info)
{
    DWORD cb, type;
    HKEY hKey;
    LONG reg_rc;

    /* host name */
    cb = sizeof(info->host_name);
    GetComputerNameA(info->host_name, &cb);

    /* arch */
#ifdef _WIN64
    strncpy(info->os_arch, "x86_64", sizeof(info->os_arch) - 1);
#else
    {
        SYSTEM_INFO nsi;
        GetNativeSystemInfo(&nsi);
        switch (nsi.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            strncpy(info->os_arch, "x86_64", sizeof(info->os_arch) - 1); break;
        case PROCESSOR_ARCHITECTURE_ARM64:
            strncpy(info->os_arch, "aarch64", sizeof(info->os_arch) - 1); break;
        case PROCESSOR_ARCHITECTURE_INTEL:
            strncpy(info->os_arch, "x86", sizeof(info->os_arch) - 1); break;
        case PROCESSOR_ARCHITECTURE_ARM:
            strncpy(info->os_arch, "arm", sizeof(info->os_arch) - 1); break;
        default:
            strncpy(info->os_arch, "unknown", sizeof(info->os_arch) - 1); break;
        }
    }
#endif

    /* os name & version from registry */
    reg_rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0, KEY_READ, &hKey);
    if (reg_rc == ERROR_SUCCESS) {
        cb = sizeof(info->os_name);
        RegQueryValueExA(hKey, "ProductName", NULL, &type,
                         (BYTE*)info->os_name, &cb);

        cb = sizeof(info->os_version);
        if (RegQueryValueExA(hKey, "DisplayVersion", NULL, &type,
                             (BYTE*)info->os_version, &cb) != ERROR_SUCCESS) {
            cb = sizeof(info->os_version);
            RegQueryValueExA(hKey, "ReleaseId", NULL, &type,
                             (BYTE*)info->os_version, &cb);
        }
        RegCloseKey(hKey);
    }

    /* kernel version via RtlGetVersion (bypasses manifest lie) */
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            RtlGetVersion_t pRtlGetVersion =
                (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
            if (pRtlGetVersion) {
                OSVERSIONINFOEXW_NT osvi;
                memset(&osvi, 0, sizeof(osvi));
                osvi.dwOSVersionInfoSize = sizeof(osvi);
                if (pRtlGetVersion(&osvi) == STATUS_SUCCESS) {
                    snprintf(info->kernel, sizeof(info->kernel),
                        "WIN32_NT %lu.%lu.%lu",
                        osvi.dwMajorVersion,
                        osvi.dwMinorVersion,
                        osvi.dwBuildNumber);
                    if (osvi.szCSDVersion[0]) {
                        size_t len = strlen(info->kernel);
                        snprintf(info->kernel + len,
                            sizeof(info->kernel) - len,
                            " %ls", osvi.szCSDVersion);
                    }
                }
            }
        }
    }

    /* uptime */
    info->uptime_seconds = GetTickCount64() / 1000;

    return 0;
}

int sys_collect_cpu_info(SysInfo *info)
{
    DWORD cb, type;
    HKEY hKey;
    LONG reg_rc;

    /* ---- CPU ---------------------------------------------------------- */
    reg_rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey);
    if (reg_rc == ERROR_SUCCESS) {
        cb = sizeof(info->cpu_name);
        RegQueryValueExA(hKey, "ProcessorNameString", NULL, &type,
                         (BYTE*)info->cpu_name, &cb);
        /* trim trailing whitespace */
        {
            size_t len = strlen(info->cpu_name);
            while (len > 0 && info->cpu_name[len - 1] == ' ')
                info->cpu_name[--len] = '\0';
        }

        DWORD mhz = 0;
        cb = sizeof(mhz);
        RegQueryValueExA(hKey, "~MHz", NULL, &type,
                         (BYTE*)&mhz, &cb);
        info->cpu_freq_max_mhz = (int)mhz;

        RegCloseKey(hKey);
    }

    /* cores & threads */
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        info->cpu_threads = (int)si.dwNumberOfProcessors;

        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf = NULL;
        DWORD len = 0;
        GetLogicalProcessorInformation(buf, &len);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            buf = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
            if (buf && GetLogicalProcessorInformation(buf, &len)) {
                DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                DWORD j;
                for (j = 0; j < count; j++) {
                    if (buf[j].Relationship == RelationProcessorCore)
                        info->cpu_cores++;
                }
            }
            free(buf);
        }
        if (info->cpu_cores == 0) info->cpu_cores = info->cpu_threads;
    }

    return 0;
}

int sys_collect_ram_info(SysInfo *info)
{
    /* ---- RAM ---------------------------------------------------------- */
    {
        MEMORYSTATUSEX memx;
        memset(&memx, 0, sizeof(memx));
        memx.dwLength = sizeof(memx);
        if (GlobalMemoryStatusEx(&memx)) {
            uint64_t pagefile_total = memx.ullTotalPageFile;
            uint64_t pagefile_avail = memx.ullAvailPageFile;

            /* ullTotalPhys excludes hardware-reserved RAM (firmware,
             * iGPU carve-out), so machines with an integrated GPU show
             * less than the installed amount.  Prefer the physically
             * installed figure; fall back to the OS-usable one. */
            {
                ULONGLONG installed_kb = 0;
                if (GetPhysicallyInstalledSystemMemory(&installed_kb) &&
                    installed_kb > 0)
                    info->memory_total_bytes = (uint64_t)installed_kb * 1024;
                else
                    info->memory_total_bytes = memx.ullTotalPhys;
            }
            info->memory_used_bytes  = memx.ullTotalPhys - memx.ullAvailPhys;

            if (pagefile_total > memx.ullTotalPhys) {
                info->swap_total_bytes = pagefile_total - memx.ullTotalPhys;
                uint64_t committed_in_use =
                    pagefile_total - pagefile_avail;
                uint64_t phys_in_use =
                    memx.ullTotalPhys - memx.ullAvailPhys;
                if (committed_in_use > phys_in_use)
                    info->swap_used_bytes = committed_in_use - phys_in_use;
                if (info->swap_used_bytes > info->swap_total_bytes)
                    info->swap_used_bytes = info->swap_total_bytes;
            }
        }
    }

    return 0;
}

int sys_collect_disk_info(SysInfo *info)
{
    /* ---- Disk --------------------------------------------------------- */
    {
        DWORD drives = GetLogicalDrives();
        int d;
        info->disk_count = 0;   /* allow re-collection on the same struct */
        for (d = 0; d < 26 && info->disk_count < SYS_INFO_MAX_DISKS; d++) {
            if (!(drives & (1 << d))) continue;

            char root[4];
            snprintf(root, sizeof(root), "%c:\\", 'A' + d);

            UINT drive_type = GetDriveTypeA(root);
            if (drive_type != DRIVE_FIXED) continue;

            /* save slot index */
            int idx = info->disk_count;

            snprintf(info->disks[idx].name,  sizeof(info->disks[idx].name),  "%c:", 'A' + d);
            snprintf(info->disks[idx].mount, sizeof(info->disks[idx].mount), "%s", root);
            info->disks[idx].is_external = 0;

            /* filesystem */
            {
                char fs[16];
                if (GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL,
                                          fs, sizeof(fs))) {
                    strncpy(info->disks[idx].filesystem, fs,
                            sizeof(info->disks[idx].filesystem) - 1);
                }
            }

            /* size / free */
            {
                ULARGE_INTEGER free_bytes, total_bytes;
                if (GetDiskFreeSpaceExA(root, &free_bytes, &total_bytes, NULL)) {
                    info->disks[idx].total_bytes = total_bytes.QuadPart;
                    info->disks[idx].used_bytes  = total_bytes.QuadPart - free_bytes.QuadPart;
                }
            }

            info->disk_count++;
        }
    }

    return 0;
}

int sys_collect_info(SysInfo *info)
{
    memset(info, 0, sizeof(SysInfo));

    sys_collect_os_info(info);
    sys_collect_cpu_info(info);
    sys_collect_ram_info(info);
    sys_collect_disk_info(info);

    return 0;
}

/* ================================================================
 *  Linux implementation
 * ================================================================ */
#elif defined(__linux__)

#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

static int read_proc_line(const char *path, const char *key,
                          char *val, size_t val_size)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            const char *v = line + strlen(key);
            while (*v == ' ' || *v == '\t' || *v == ':') v++;
            size_t len = strlen(v);
            while (len > 0 && (v[len-1] == '\n' || v[len-1] == ' '))
                len--;
            len = len < val_size - 1 ? len : val_size - 1;
            memcpy(val, v, len);
            val[len] = '\0';
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static long long read_proc_val(const char *path, const char *key)
{
    char buf[64];
    if (read_proc_line(path, key, buf, sizeof(buf)) == 0)
        return strtoll(buf, NULL, 10);
    return -1;
}

int sys_collect_os_info(SysInfo *info)
{
    /* host name */
    gethostname(info->host_name, sizeof(info->host_name));

    /* os name & version from /etc/os-release */
    {
        FILE *f = fopen("/etc/os-release", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                    char *v = line + 12;
                    if (*v == '"') { v++; v[strcspn(v, "\"")] = '\0'; }
                    strncpy(info->os_name, v, sizeof(info->os_name) - 1);
                } else if (strncmp(line, "VERSION_ID=", 11) == 0) {
                    char *v = line + 11;
                    if (*v == '"') { v++; v[strcspn(v, "\"")] = '\0'; }
                    strncpy(info->os_version, v, sizeof(info->os_version) - 1);
                } else if (strncmp(line, "ID=", 3) == 0 && !info->os_name[0]) {
                    char *v = line + 3;
                    if (*v == '"') { v++; v[strcspn(v, "\"")] = '\0'; }
                    strncpy(info->os_name, v, sizeof(info->os_name) - 1);
                }
            }
            fclose(f);
        }
    }

    /* uname */
    {
        struct utsname u;
        if (uname(&u) == 0) {
            snprintf(info->kernel, sizeof(info->kernel),
                     "%s %s", u.sysname, u.release);
            strncpy(info->os_arch, u.machine, sizeof(info->os_arch) - 1);
        }
    }

    /* uptime */
    {
        FILE *f = fopen("/proc/uptime", "r");
        if (f) {
            double up;
            if (fscanf(f, "%lf", &up) == 1)
                info->uptime_seconds = (uint64_t)up;
            fclose(f);
        }
    }

    return 0;
}

int sys_collect_cpu_info(SysInfo *info)
{
    /* ---- CPU ---------------------------------------------------------- */
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[256];
            int physical_ids[64] = {0};
            int n_physical = 0;
            int n_cores = 0, n_threads = 0;
            long long freq = 0;
            while (fgets(line, sizeof(line), f)) {
                long long val;
                if (strncmp(line, "model name", 10) == 0 && !info->cpu_name[0]) {
                    const char *v = strchr(line, ':');
                    if (v) {
                        v++;
                        while (*v == ' ') v++;
                        size_t len = strlen(v);
                        while (len > 0 && v[len-1] == '\n') len--;
                        len = len < sizeof(info->cpu_name) - 1
                            ? len : sizeof(info->cpu_name) - 1;
                        memcpy(info->cpu_name, v, len);
                        info->cpu_name[len] = '\0';
                    }
                } else if (strncmp(line, "cpu cores", 9) == 0) {
                    val = strtoll(strchr(line, ':') + 1, NULL, 10);
                    if (val > n_cores) n_cores = (int)val;
                } else if (strncmp(line, "siblings", 8) == 0) {
                    val = strtoll(strchr(line, ':') + 1, NULL, 10);
                    if (val > n_threads) n_threads = (int)val;
                } else if (strncmp(line, "cpu MHz", 7) == 0) {
                    val = strtoll(strchr(line, ':') + 1, NULL, 10);
                    if (val > freq) freq = val;
                } else if (strncmp(line, "physical id", 11) == 0) {
                    val = strtoll(strchr(line, ':') + 1, NULL, 10);
                    int found = 0, k;
                    for (k = 0; k < n_physical; k++) {
                        if (physical_ids[k] == (int)val) { found = 1; break; }
                    }
                    if (!found && n_physical < 64)
                        physical_ids[n_physical++] = (int)val;
                }
            }
            fclose(f);

            info->cpu_cores   = n_cores    > 0 ? n_cores    : (n_physical > 0 ? n_physical : 1);
            info->cpu_threads = n_threads  > 0 ? n_threads  : info->cpu_cores;
            info->cpu_freq_max_mhz = (int)(freq > 0 ? freq : 0);
        }
    }

    return 0;
}

/* MemTotal excludes kernel-reserved and hardware-reserved RAM (firmware,
 * iGPU stolen memory), so machines with an integrated GPU show less than
 * the installed amount.  Sum the online blocks under
 * /sys/devices/system/memory (same approach as lsmem) to get the
 * physically installed figure.  Returns 0 when unavailable. */
static uint64_t linux_installed_ram_bytes(void)
{
    uint64_t block = 0, total = 0;
    char buf[64];
    FILE *f;
    DIR *d;
    struct dirent *de;

    f = fopen("/sys/devices/system/memory/block_size_bytes", "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    block = strtoull(buf, NULL, 0);   /* value is hex, e.g. 0x8000000 */
    if (block == 0) return 0;

    d = opendir("/sys/devices/system/memory");
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        char path[512];
        if (strncmp(de->d_name, "memory", 6) != 0 ||
            !isdigit((unsigned char)de->d_name[6]))
            continue;
        /* skip offline blocks; a missing "online" file means the block
         * is not hot-removable, i.e. permanently online */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/memory/%s/online", de->d_name);
        f = fopen(path, "r");
        if (f) {
            int c = fgetc(f);
            fclose(f);
            if (c == '0') continue;
        }
        total += block;
    }
    closedir(d);
    return total;
}

int sys_collect_ram_info(SysInfo *info)
{
    /* ---- RAM ---------------------------------------------------------- */
    {
        long long total = read_proc_val("/proc/meminfo", "MemTotal");
        long long avail = read_proc_val("/proc/meminfo", "MemAvailable");
        long long swapt = read_proc_val("/proc/meminfo", "SwapTotal");
        long long swapf = read_proc_val("/proc/meminfo", "SwapFree");

        if (total > 0) {
            uint64_t installed = linux_installed_ram_bytes();
            info->memory_total_bytes = installed > 0
                ? installed : (uint64_t)total * 1024;
            /* MemAvailable is absent on kernels < 3.14 */
            if (avail < 0)
                avail = read_proc_val("/proc/meminfo", "MemFree");
            if (avail > 0)
                info->memory_used_bytes = (uint64_t)(total - avail) * 1024;
        }
        if (swapt > 0) {
            info->swap_total_bytes = (uint64_t)swapt * 1024;
            if (swapf >= 0)
                info->swap_used_bytes = (uint64_t)(swapt - swapf) * 1024;
        }
    }

    return 0;
}

int sys_collect_disk_info(SysInfo *info)
{
    /* ---- Disk --------------------------------------------------------- */
    {
        FILE *f = setmntent("/proc/mounts", "r");
        info->disk_count = 0;   /* allow re-collection on the same struct */
        if (f) {
            struct mntent *ent;
            /* full device paths already listed, for de-duplication */
            char seen[SYS_INFO_MAX_DISKS][128];
            int n_seen = 0;
            const char *real_fs[] = {
                "ext2","ext3","ext4","xfs","btrfs","zfs",
                "ntfs","ntfs3","vfat","fuseblk","f2fs",
                "jfs","reiserfs","hfs","hfsplus","apfs",NULL
            };
            while ((ent = getmntent(f))
                   && info->disk_count < SYS_INFO_MAX_DISKS) {
                const char **fs;
                int ok = 0, dup = 0, j;
                for (fs = real_fs; *fs; fs++) {
                    if (strcmp(ent->mnt_type, *fs) == 0) {
                        ok = 1; break;
                    }
                }
                if (!ok) continue;

                /* the same device may appear many times in /proc/mounts
                 * (btrfs subvolumes, bind mounts) -- keep only the first */
                for (j = 0; j < n_seen; j++) {
                    if (strcmp(seen[j], ent->mnt_fsname) == 0) {
                        dup = 1; break;
                    }
                }
                if (dup) continue;

                /* skip unreachable/stale mounts instead of emitting a
                 * bogus 0 B / 0 B entry */
                struct statvfs sv;
                if (statvfs(ent->mnt_dir, &sv) != 0) continue;

                strncpy(seen[n_seen], ent->mnt_fsname,
                        sizeof(seen[n_seen]) - 1);
                seen[n_seen][sizeof(seen[n_seen]) - 1] = '\0';
                n_seen++;

                int idx = info->disk_count;
                strncpy(info->disks[idx].filesystem, ent->mnt_type,
                        sizeof(info->disks[idx].filesystem) - 1);
                strncpy(info->disks[idx].mount, ent->mnt_dir,
                        sizeof(info->disks[idx].mount) - 1);
                /* name: pick last path component or device name */
                {
                    const char *dev = ent->mnt_fsname;
                    const char *slash = strrchr(dev, '/');
                    if (slash) dev = slash + 1;
                    strncpy(info->disks[idx].name, dev,
                            sizeof(info->disks[idx].name) - 1);
                }

                info->disks[idx].total_bytes =
                    (uint64_t)sv.f_frsize * sv.f_blocks;
                info->disks[idx].used_bytes =
                    (uint64_t)sv.f_frsize * (sv.f_blocks - sv.f_bfree);

                info->disk_count++;
            }
            endmntent(f);
        }
    }

    return 0;
}

int sys_collect_info(SysInfo *info)
{
    memset(info, 0, sizeof(SysInfo));

    sys_collect_os_info(info);
    sys_collect_cpu_info(info);
    sys_collect_ram_info(info);
    sys_collect_disk_info(info);

    return 0;
}

#endif /* _WIN32 / __linux__ */
