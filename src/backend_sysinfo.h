#ifndef BACKEND_SYSINFO_H
#define BACKEND_SYSINFO_H

#include "sys_info.h"

int sys_collect_info(SysInfo *info);
int sys_collect_os_info(SysInfo *info);
int sys_collect_cpu_info(SysInfo *info);
int sys_collect_ram_info(SysInfo *info);
int sys_collect_disk_info(SysInfo *info);

#endif
