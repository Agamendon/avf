/*++

Copyright (c) 1989-2002  Microsoft Corporation
Modified for AV Filter functionality

Module Name:

    avfLog.c

Abstract:

    This file contains logging utility functions for the user-mode
    component of the AV Filter.

Environment:

    User mode

--*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "avf.h"

//
//  Log file handle
//

static HANDLE gLogFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION gLogLock;
static BOOL gLogLockInitialized = FALSE;


BOOL
InitializeLogging(
    _In_opt_ PCWSTR LogFilePath
    )
/*++

Routine Description:

    Initializes the logging subsystem.

Arguments:

    LogFilePath - Path to the log file, or NULL for console only.

Return Value:

    TRUE if successful.

--*/
{
    if (!gLogLockInitialized) {
        InitializeCriticalSection(&gLogLock);
        gLogLockInitialized = TRUE;
    }

    if (LogFilePath != NULL) {
        gLogFile = CreateFileW(
                        LogFilePath,
                        GENERIC_WRITE,
                        FILE_SHARE_READ,
                        NULL,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);

        if (gLogFile == INVALID_HANDLE_VALUE) {
            wprintf(L"WARNING: Could not create log file: %s\n", LogFilePath);
            return FALSE;
        }

        //
        //  Write UTF-16 BOM
        //

        WCHAR bom = 0xFEFF;
        DWORD written;
        WriteFile(gLogFile, &bom, sizeof(bom), &written, NULL);
    }

    return TRUE;
}


VOID
ShutdownLogging(
    VOID
    )
/*++

Routine Description:

    Shuts down the logging subsystem.

Arguments:

    None.

Return Value:

    None.

--*/
{
    if (gLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(gLogFile);
        gLogFile = INVALID_HANDLE_VALUE;
    }

    if (gLogLockInitialized) {
        DeleteCriticalSection(&gLogLock);
        gLogLockInitialized = FALSE;
    }
}


VOID
LogFileAccess(
    _In_ ULONG ProcessId,
    _In_ PCWSTR ProcessName,
    _In_ PCWSTR FileName,
    _In_ UCHAR MajorFunction
    )
/*++

Routine Description:

    Logs a file access event to the console and optionally to a file.

Arguments:

    ProcessId - Process ID of the accessing process.
    ProcessName - Name of the accessing process.
    FileName - Name of the accessed file.
    MajorFunction - IRP major function (read/write).

Return Value:

    None.

--*/
{
    SYSTEMTIME st;
    WCHAR buffer[2048];
    int len;
    DWORD written;

    GetLocalTime(&st);

    len = swprintf_s(buffer, ARRAYSIZE(buffer),
                     L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] PID: %5lu  Process: %-20s  File: %s\r\n",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     MajorFunction == IRP_MJ_READ ? L"READ " : L"WRITE",
                     ProcessId,
                     ProcessName,
                     FileName);

    if (len > 0) {

        if (gLogLockInitialized) {
            EnterCriticalSection(&gLogLock);
        }

        //
        //  Write to console
        //

        wprintf(L"%s", buffer);

        //
        //  Write to log file if open
        //

        if (gLogFile != INVALID_HANDLE_VALUE) {
            WriteFile(gLogFile, buffer, (DWORD)(len * sizeof(WCHAR)), &written, NULL);
        }

        if (gLogLockInitialized) {
            LeaveCriticalSection(&gLogLock);
        }
    }
}


VOID
LogMessage(
    _In_ PCWSTR Format,
    ...
    )
/*++

Routine Description:

    Logs a formatted message.

Arguments:

    Format - Printf-style format string.
    ... - Format arguments.

Return Value:

    None.

--*/
{
    va_list args;
    WCHAR buffer[1024];
    int len;
    DWORD written;

    va_start(args, Format);
    len = vswprintf_s(buffer, ARRAYSIZE(buffer), Format, args);
    va_end(args);

    if (len > 0) {

        if (gLogLockInitialized) {
            EnterCriticalSection(&gLogLock);
        }

        wprintf(L"%s", buffer);

        if (gLogFile != INVALID_HANDLE_VALUE) {
            WriteFile(gLogFile, buffer, (DWORD)(len * sizeof(WCHAR)), &written, NULL);
        }

        if (gLogLockInitialized) {
            LeaveCriticalSection(&gLogLock);
        }
    }
}
