/*++

Copyright (c) 1989-2002  Microsoft Corporation
Modified for AV Filter functionality

Module Name:

    avfLib.c

Abstract:

    This contains library support routines for the AVF minifilter driver.

Environment:

    Kernel mode

--*/

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>
#include "avf.h"

//
//  Declare PsGetProcessImageFileName (not in public headers)
//

NTKERNELAPI
PUCHAR
PsGetProcessImageFileName(
    _In_ PEPROCESS Process
    );

//
//  Function declarations
//

NTSTATUS
AvfGetProcessName(
    _Out_writes_bytes_(BufferSize) PWCHAR ProcessName,
    _In_ ULONG BufferSize
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, AvfGetProcessName)
#endif

//
//  Pool tag for allocations
//

#define AVF_NAME_TAG 'NfvA'


NTSTATUS
AvfGetProcessName(
    _Out_writes_bytes_(BufferSize) PWCHAR ProcessName,
    _In_ ULONG BufferSize
    )
/*++

Routine Description:

    Gets the current process name.

Arguments:

    ProcessName - Buffer to receive the process name (Unicode).
    BufferSize - Size of the buffer in bytes.

Return Value:

    STATUS_SUCCESS if successful.

--*/
{
    PEPROCESS process;
    PUCHAR imageName;
    ANSI_STRING ansiName;
    UNICODE_STRING unicodeName;
    NTSTATUS status;

    PAGED_CODE();

    if (ProcessName == NULL || BufferSize < sizeof(WCHAR)) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(ProcessName, BufferSize);

    process = PsGetCurrentProcess();
    if (process == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    imageName = PsGetProcessImageFileName(process);
    if (imageName == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    //  Convert ANSI to Unicode
    //

    RtlInitAnsiString(&ansiName, (PCSZ)imageName);

    unicodeName.Buffer = ProcessName;
    unicodeName.MaximumLength = (USHORT)(BufferSize - sizeof(WCHAR));
    unicodeName.Length = 0;

    status = RtlAnsiStringToUnicodeString(&unicodeName, &ansiName, FALSE);

    return status;
}


BOOLEAN
AvfIsFileProtected(
    _In_ PCUNICODE_STRING FileName
    )
/*++

Routine Description:

    Checks if a file should be protected/monitored.
    Currently returns TRUE for all files - filtering can be done in userspace.

Arguments:

    FileName - Name of the file to check.

Return Value:

    TRUE if the file should be monitored, FALSE otherwise.

--*/
{
    UNREFERENCED_PARAMETER(FileName);

    //
    //  For now, monitor all files. The userspace application
    //  will filter based on protected file list.
    //

    return TRUE;
}
