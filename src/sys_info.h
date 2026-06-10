#ifndef SYS_INFO_H
#define SYS_INFO_H

#include <stdint.h>

#define SYS_INFO_MAX_DISKS 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* OS */
    char     os_name[128];
    char     os_version[64];
    char     os_arch[16];
    char     host_name[128];
    char     kernel[128];
    uint64_t uptime_seconds;

    /* CPU */
    char cpu_name[128];
    int  cpu_cores;
    int  cpu_threads;
    int  cpu_freq_max_mhz;

    /* RAM */
    uint64_t memory_total_bytes;
    uint64_t memory_used_bytes;
    uint64_t swap_total_bytes;
    uint64_t swap_used_bytes;

    /* Disk */
    int disk_count;
    struct {
        char     name[16];
        char     mount[64];
        char     filesystem[16];
        uint64_t total_bytes;
        uint64_t used_bytes;
        int      is_external;
    } disks[SYS_INFO_MAX_DISKS];
} SysInfo;

#ifdef __cplusplus
}
#endif

#endif /* SYS_INFO_H */
