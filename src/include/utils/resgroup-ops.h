/*-------------------------------------------------------------------------
 *
 * resgroup-ops.h
 *	  GPDB resource group definitions.
 *
 * Copyright (c) 2017 Pivotal Software, Inc.
 *
 *
 * IDENTIFICATION
 *	    src/include/utils/resgroup-ops.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RES_GROUP_OPS_H
#define RES_GROUP_OPS_H

#define RESGROUP_ROOT_ID (InvalidOid)
/*
 * Interfaces for OS dependent operations
 */

extern const char * ResGroupOps_Name(void);
extern void ResGroupOps_Bless(void);
extern void ResGroupOps_Init(void);
extern void ResGroupOps_AdjustGUCs(void);
extern void ResGroupOps_CreateGroup(Oid group);
extern void ResGroupOps_DestroyGroup(Oid group);
extern void ResGroupOps_AssignGroup(Oid group, int pid);
extern int ResGroupOps_LockGroup(Oid group, const char *comp, bool block);
extern void ResGroupOps_UnLockGroup(Oid group, int fd);
extern void ResGroupOps_SetCpuRateLimit(Oid group, int cpu_rate_limit);
extern void ResGroupOps_SetMemoryLimitByRate(Oid group, int memory_limit);
extern void ResGroupOps_SetMemoryLimitByValue(Oid group, int32 memory_limit);
extern int64 ResGroupOps_GetCpuUsage(Oid group);
extern int32 ResGroupOps_GetMemoryUsage(Oid group);
extern int32 ResGroupOps_GetMemoryLimit(Oid group);
extern int ResGroupOps_GetCpuCores(void);
extern int ResGroupOps_GetTotalMemory(void);
extern char * buildPath(Oid group, const char *base, const char *comp, const char *prop, char *path, size_t pathsize);
extern int lockDir(const char *path, bool block);
#endif   /* RES_GROUP_OPS_H */
