// Copyright 2018 plutoo
#include <malloc.h>
#include "types.h"
#include "result.h"
#include "runtime/env.h"
#include "arm/cache.h"
#include "kernel/svc.h"
#include "kernel/detect.h"
#include "kernel/virtmem.h"
#include "kernel/jit.h"

Result jitCreate(Jit* j, size_t size)
{
    JitType type;

    // Use new CodeMemory object introduced in [4.0.0+], if available.
    // On [5.0.0+] this is only usable with a kernel patch, as svcControlCodeMemory now errors if it's used under the same process which owns the object.
    if (kernelAbove400() && envIsSyscallHinted(0x4B) && envIsSyscallHinted(0x4C)
        && (!kernelAbove500() || detectJitKernelPatch())) {
	type = JitType_CodeMemory;
    }
    // Fall back to JitType_SetProcessMemoryPermission if available.
    else if (envIsSyscallHinted(0x73) && envIsSyscallHinted(0x77) && envIsSyscallHinted(0x78)
             && (envGetOwnProcessHandle() != INVALID_HANDLE)) {
        type = JitType_SetProcessMemoryPermission;
    }
    else {
        // Jit is unavailable. :(
        return MAKERESULT(Module_Libnx, LibnxError_JitUnavailable);
    }

    size = (size + 0xFFF) &~ 0xFFF;

    void* src_addr = memalign(0x1000, size);

    if (src_addr == NULL)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    j->type = type;
    j->size = size;
    j->src_addr = src_addr;
    j->rx_addr = virtmemReserve(j->size);
    j->handle = INVALID_HANDLE;
    j->is_executable = 0;

    Result rc = 0;

    switch (j->type)
    {
    case JitType_SetProcessMemoryPermission:
        j->rw_addr = j->src_addr;
        break;

    case JitType_CodeMemory:
        j->rw_addr = virtmemReserve(j->size);

        rc = svcCreateCodeMemory(&j->handle, j->src_addr, j->size);
        if (R_SUCCEEDED(rc))
        {
            rc = svcControlCodeMemory(j->handle, CodeMapOperation_MapOwner, j->rw_addr, j->size, Perm_Rw);
            if (R_SUCCEEDED(rc))
            {
                rc = svcControlCodeMemory(j->handle, CodeMapOperation_MapSlave, j->rx_addr, j->size, Perm_Rx);

                if (R_FAILED(rc)) {
                    svcControlCodeMemory(j->handle, CodeMapOperation_UnmapOwner, j->rw_addr, j->size, 0);
                }
            }

            if (R_FAILED(rc)) {
                svcCloseHandle(j->handle);
                j->handle = INVALID_HANDLE;
            }
        }

        if (R_FAILED(rc)) {
            virtmemFree(j->rw_addr, j->size);
            j->rw_addr = NULL;
        }

        break;
    }

    if (R_FAILED(rc)) {
        virtmemFree(j->rx_addr, j->size);
        free(j->src_addr);
        j->src_addr = NULL;
    }

    return rc;
}

Result jitTransitionToWritable(Jit* j)
{
    Result rc = 0;

    switch (j->type) {
    case JitType_SetProcessMemoryPermission:
        if (j->is_executable) rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64) j->rx_addr, (u64) j->src_addr, j->size);
        break;

    case JitType_CodeMemory:
        // No need to do anything.
        break;
    }

    if (R_SUCCEEDED(rc)) j->is_executable = 0;

    return rc;
}

Result jitTransitionToExecutable(Jit* j)
{
    Result rc = 0;

    switch (j->type) {
    case JitType_SetProcessMemoryPermission:
        if (!j->is_executable) {
            rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64) j->rx_addr, (u64) j->src_addr, j->size);

            if (R_SUCCEEDED(rc)) {
                rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), (u64) j->rx_addr, j->size, Perm_Rx);

                if (R_FAILED(rc)) {
                    jitTransitionToWritable(j);
                }
            }
        }
        break;

    case JitType_CodeMemory:
        armDCacheFlush(j->rw_addr, j->size);
        armICacheInvalidate(j->rx_addr, j->size);
        break;
    }

    if (R_SUCCEEDED(rc)) j->is_executable = 1;

    return rc;
}

Result jitClose(Jit* j)
{
    Result rc = 0;

    switch (j->type)
    {
    case JitType_SetProcessMemoryPermission:
        rc = jitTransitionToWritable(j);

        if (R_SUCCEEDED(rc)) {
            virtmemFree(j->rx_addr, j->size);
        }
        break;

    case JitType_CodeMemory:
        rc = svcControlCodeMemory(j->handle, CodeMapOperation_UnmapOwner, j->rw_addr, j->size, 0);

        if (R_SUCCEEDED(rc)) {
            virtmemFree(j->rw_addr, j->size);

            rc = svcControlCodeMemory(j->handle, CodeMapOperation_UnmapSlave, j->rx_addr, j->size, 0);

            if (R_SUCCEEDED(rc)) {
                virtmemFree(j->rx_addr, j->size);
                svcCloseHandle(j->handle);
            }
        }
        break;
    }

    if (R_SUCCEEDED(rc)) {
        if (j->src_addr != NULL) {
            free(j->src_addr);
            j->src_addr = NULL;
        }
    }
    return rc;
}
