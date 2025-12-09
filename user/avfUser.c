/*++

Copyright (c) 1989-2002  Microsoft Corporation
Modified for AV Filter functionality

Module Name:

    avfUser.c

Abstract:

    This file contains the main function for the user-mode component
    of the AV Filter. It connects to the kernel minifilter and receives
    notifications about file accesses.

Environment:

    User mode

--*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <fltUser.h>
#include <dontuse.h>
#include "avf.h"

//
//  Global variables
//

HANDLE gPort = INVALID_HANDLE_VALUE;
HANDLE gConsultantPipe = INVALID_HANDLE_VALUE;
volatile BOOLEAN gRunning = TRUE;
BOOLEAN gConsultantConnected = FALSE;

//
//  Protected files list - stores NT device paths for comparison
//

#define MAX_PROTECTED_FILES 100
WCHAR gProtectedFiles[MAX_PROTECTED_FILES][AVF_MAX_PATH];
ULONG gProtectedFileCount = 0;

//
//  Function prototypes
//

BOOL
ConnectToConsultant(
    VOID
    );

BOOL
QueryConsultant(
    _In_ PAVF_FILE_NOTIFICATION pNotification,
    _Out_ PAVF_CONSULTANT_RESPONSE pResponse
    );

BOOL
ConvertToNtPath(
    _In_ PCWSTR Win32Path,
    _Out_writes_(NtPathSize) PWSTR NtPath,
    _In_ ULONG NtPathSize
    );

BOOL
AddProtectedFile(
    _In_ PCWSTR FilePath
    );

BOOL
IsFileProtected(
    _In_ PCWSTR FilePath
    );

VOID
PrintUsage(
    VOID
    );

BOOL WINAPI
ConsoleCtrlHandler(
    DWORD CtrlType
    );


int
wmain(
    _In_ int argc,
    _In_reads_(argc) WCHAR *argv[]
    )
/*++

Routine Description:

    Main entry point for the userspace listener application.
    Connects to the minifilter and listens for file access notifications.

Arguments:

    argc - Argument count.
    argv - Argument vector.

Return Value:

    Exit code.

--*/
{
HRESULT hr;
UCHAR messageBuffer[sizeof(FILTER_MESSAGE_HEADER) + sizeof(AVF_FILE_NOTIFICATION)];
PFILTER_MESSAGE_HEADER pMessage;
PAVF_FILE_NOTIFICATION pNotification;
int i;

//
//  Reply structure with FILTER_REPLY_HEADER
//
struct {
    FILTER_REPLY_HEADER Header;
    AVF_REPLY Reply;
} replyBuffer;

    wprintf(L"AV Filter - File Access Monitor\n");
    wprintf(L"================================\n\n");

    //
    //  Parse command line arguments
    //

    if (argc < 2) {
        wprintf(L"Usage: %s <file1> [file2] [file3] ...\n", argv[0]);
        wprintf(L"\nSpecify files to monitor. When any process accesses these files,\n");
        wprintf(L"the process PID and name will be displayed.\n\n");
        wprintf(L"Example: %s C:\\important.txt C:\\secret.doc\n\n", argv[0]);
    }

    //
    //  Add protected files from command line
    //

    for (i = 1; i < argc; i++) {
        if (AddProtectedFile(argv[i])) {
            wprintf(L"Monitoring: %s\n", argv[i]);
        }
    }

    if (gProtectedFileCount == 0) {
        wprintf(L"\nNo files specified - will display ALL file access events.\n");
        wprintf(L"Press Ctrl+C to exit.\n\n");
    } else {
        wprintf(L"\nMonitoring %lu file(s). Press Ctrl+C to exit.\n\n", gProtectedFileCount);
    }

    //
    //  Set up console control handler for clean shutdown
    //

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    //
    //  Connect to the minifilter
    //

    hr = FilterConnectCommunicationPort(
            AVF_PORT_NAME,
            0,
            NULL,
            0,
            NULL,
            &gPort);

    if (FAILED(hr)) {
        wprintf(L"ERROR: Failed to connect to filter (0x%08X)\n", hr);
        wprintf(L"Make sure the avf driver is loaded.\n");
        wprintf(L"Run: fltmc load avf\n");
        return 1;
    }

    wprintf(L"Connected to avf filter.\n");

    //
    //  Try to connect to security consultant
    //

    if (ConnectToConsultant()) {
        wprintf(L"Connected to security consultant.\n");
    } else {
        wprintf(L"Security consultant not available - will allow all operations.\n");
        wprintf(L"Start avfConsultant.exe to enable security decisions.\n");
    }

    wprintf(L"\nWaiting for file access events...\n\n");

    //
    //  Main loop - receive notifications from the filter
    //

    pMessage = (PFILTER_MESSAGE_HEADER)messageBuffer;

    while (gRunning) {

        hr = FilterGetMessage(
                gPort,
                pMessage,
                sizeof(messageBuffer),
                NULL);

        if (FAILED(hr)) {
            if (hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) {
                //
                //  Port was closed, exit normally
                //
                break;
            }
            wprintf(L"ERROR: FilterGetMessage failed (0x%08X)\n", hr);
            break;
        }

        //
        //  Extract notification from message
        //

        pNotification = (PAVF_FILE_NOTIFICATION)(pMessage + 1);

        //
        //  Check if this file is in our protected list
        //

        if (gProtectedFileCount == 0 || IsFileProtected(pNotification->FileName)) {

            //
            //  Print the file access information
            //

            wprintf(L"[%s] PID: %5lu  Process: %-20s  File: %s\n",
                    pNotification->MajorFunction == IRP_MJ_CREATE ? L"OPEN " :
                    pNotification->MajorFunction == IRP_MJ_READ ? L"READ " : L"WRITE",
                    pNotification->ProcessId,
                    pNotification->ProcessName,
                    pNotification->FileName);

            //
            //  Query security consultant - try to connect if not connected
            //

            if (!gConsultantConnected) {
                //
                //  Try to reconnect to consultant (may have started after us)
                //
                if (ConnectToConsultant()) {
                    wprintf(L"  -> Connected to security consultant\n");
                }
            }

            if (gConsultantConnected) {
                AVF_CONSULTANT_RESPONSE response;

                if (QueryConsultant(pNotification, &response)) {
                    if (response.Decision == AVF_DECISION_BLOCK) {
                        wprintf(L"  -> BLOCKED by consultant (reason code: %lu)\n", response.Reason);
                        replyBuffer.Reply.BlockOperation = 1;
                    } else {
                        wprintf(L"  -> ALLOWED by consultant\n");
                        replyBuffer.Reply.BlockOperation = 0;
                    }
                } else {
                    //
                    //  Consultant disconnected or error - allow by default
                    //
                    wprintf(L"  -> Consultant disconnected, allowing\n");
                    replyBuffer.Reply.BlockOperation = 0;
                }
            } else {
                replyBuffer.Reply.BlockOperation = 0;
            }
        } else {
            //
            //  Not a protected file - allow
            //
            replyBuffer.Reply.BlockOperation = 0;
        }

        //
        //  Send reply back to kernel
        //

        replyBuffer.Header.Status = 0;
        replyBuffer.Header.MessageId = pMessage->MessageId;

        hr = FilterReplyMessage(
                gPort,
                &replyBuffer.Header,
                sizeof(replyBuffer.Header) + sizeof(replyBuffer.Reply));

        if (FAILED(hr)) {
            wprintf(L"WARNING: FilterReplyMessage failed (0x%08X)\n", hr);
        }
    }

    //
    //  Cleanup
    //

    if (gConsultantPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
    }

    if (gPort != INVALID_HANDLE_VALUE) {
        CloseHandle(gPort);
        gPort = INVALID_HANDLE_VALUE;
    }

    wprintf(L"\nExiting...\n");
    return 0;
}


BOOL
ConvertToNtPath(
    _In_ PCWSTR Win32Path,
    _Out_writes_(NtPathSize) PWSTR NtPath,
    _In_ ULONG NtPathSize
    )
/*++

Routine Description:

    Converts a Win32 path (C:\...) to NT device path (\Device\HarddiskVolumeX\...).

Arguments:

    Win32Path - Win32 path to convert.
    NtPath - Buffer to receive NT path.
    NtPathSize - Size of buffer in characters.

Return Value:

    TRUE if successful, FALSE otherwise.

--*/
{
    WCHAR fullPath[MAX_PATH];
    WCHAR drive[3];
    WCHAR deviceName[MAX_PATH];
    DWORD result;
    size_t deviceLen;
    size_t pathLen;

    //
    //  Get full path name first
    //

    result = GetFullPathNameW(Win32Path, MAX_PATH, fullPath, NULL);
    if (result == 0 || result >= MAX_PATH) {
        return FALSE;
    }

    //
    //  Extract drive letter (e.g., "C:")
    //

    if (fullPath[1] != L':') {
        //
        //  Not a drive-letter path, just copy as-is
        //
        wcsncpy_s(NtPath, NtPathSize, fullPath, _TRUNCATE);
        _wcsupr_s(NtPath, NtPathSize);
        return TRUE;
    }

    drive[0] = fullPath[0];
    drive[1] = L':';
    drive[2] = L'\0';

    //
    //  Query the DOS device name to get NT device path
    //

    result = QueryDosDeviceW(drive, deviceName, MAX_PATH);
    if (result == 0) {
        return FALSE;
    }

    //
    //  Build NT path: \Device\HarddiskVolumeX + rest of path after drive letter
    //

    deviceLen = wcslen(deviceName);
    pathLen = wcslen(fullPath + 2);  // Skip "C:"

    if (deviceLen + pathLen + 1 >= NtPathSize) {
        return FALSE;
    }

    wcscpy_s(NtPath, NtPathSize, deviceName);
    wcscat_s(NtPath, NtPathSize, fullPath + 2);  // Append path after "C:"

    //
    //  Convert to uppercase for case-insensitive comparison
    //

    _wcsupr_s(NtPath, NtPathSize);

    return TRUE;
}


BOOL
AddProtectedFile(
    _In_ PCWSTR FilePath
    )
/*++

Routine Description:

    Adds a file to the protected files list.

Arguments:

    FilePath - Path to the file to protect.

Return Value:

    TRUE if successful, FALSE otherwise.

--*/
{
    if (gProtectedFileCount >= MAX_PROTECTED_FILES) {
        wprintf(L"WARNING: Maximum protected file limit reached (%d)\n", MAX_PROTECTED_FILES);
        return FALSE;
    }

    //
    //  Convert Win32 path to NT device path for comparison with kernel paths
    //

    if (!ConvertToNtPath(FilePath, 
                         gProtectedFiles[gProtectedFileCount], 
                         AVF_MAX_PATH)) {
        wprintf(L"WARNING: Failed to convert path: %s\n", FilePath);
        return FALSE;
    }

    gProtectedFileCount++;
    return TRUE;
}


BOOL
IsFileProtected(
    _In_ PCWSTR FilePath
    )
/*++

Routine Description:

    Checks if a file is in the protected files list.

Arguments:

    FilePath - NT device path to check (from kernel notification).

Return Value:

    TRUE if the file is protected, FALSE otherwise.

--*/
{
    WCHAR upperPath[AVF_MAX_PATH];
    ULONG i;
    size_t len;

    //
    //  Convert kernel path to uppercase for comparison
    //

    len = wcslen(FilePath);
    if (len >= AVF_MAX_PATH) {
        len = AVF_MAX_PATH - 1;
    }

    wcsncpy_s(upperPath, AVF_MAX_PATH, FilePath, len);
    upperPath[len] = L'\0';
    _wcsupr_s(upperPath, AVF_MAX_PATH);

    //
    //  Check against protected files list (exact match)
    //

    for (i = 0; i < gProtectedFileCount; i++) {
        if (wcscmp(upperPath, gProtectedFiles[i]) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}


BOOL WINAPI
ConsoleCtrlHandler(
    DWORD CtrlType
    )
/*++

Routine Description:

    Console control handler for clean shutdown.

Arguments:

    CtrlType - Type of control signal.

Return Value:

    TRUE if handled.

--*/
{
    UNREFERENCED_PARAMETER(CtrlType);

    gRunning = FALSE;

    //
    //  Cancel pending I/O on the port
    //

    if (gPort != INVALID_HANDLE_VALUE) {
        CancelIoEx(gPort, NULL);
    }

    return TRUE;
}


BOOL
ConnectToConsultant(
    VOID
    )
/*++

Routine Description:

    Connects to the security consultant process via named pipe and performs handshake.

Arguments:

    None.

Return Value:

    TRUE if connected and handshake succeeded, FALSE otherwise.

--*/
{
    AVF_CONSULTANT_REQUEST handshakeRequest;
    AVF_CONSULTANT_RESPONSE handshakeResponse;
    DWORD bytesWritten;
    DWORD bytesRead;
    DWORD mode;

    //
    //  Try to connect to the consultant's named pipe
    //

    wprintf(L"  [Handshake] Connecting to consultant pipe...\n");

    gConsultantPipe = CreateFileW(
                        AVF_CONSULTANT_PIPE_NAME,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        0,
                        NULL);

    if (gConsultantPipe == INVALID_HANDLE_VALUE) {
        wprintf(L"  [Handshake] Failed to open pipe (error %lu)\n", GetLastError());
        gConsultantConnected = FALSE;
        return FALSE;
    }

    wprintf(L"  [Handshake] Pipe opened, setting message mode...\n");

    //
    //  Set pipe to message mode
    //

    mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(gConsultantPipe, &mode, NULL, NULL)) {
        wprintf(L"  [Handshake] Failed to set message mode (error %lu)\n", GetLastError());
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    //
    //  Send handshake request (RequestId = 0, Operation = 0xFF for handshake)
    //

    wprintf(L"  [Handshake] Sending handshake request...\n");

    RtlZeroMemory(&handshakeRequest, sizeof(handshakeRequest));
    handshakeRequest.Version = AVF_CONSULTANT_PROTOCOL_VERSION;
    handshakeRequest.RequestId = 0;  // Special ID for handshake
    handshakeRequest.ProcessId = GetCurrentProcessId();
    handshakeRequest.Operation = 0xFF;  // Special operation for handshake
    wcscpy_s(handshakeRequest.ProcessName, 260, L"AVF_HANDSHAKE");
    wcscpy_s(handshakeRequest.FileName, AVF_MAX_PATH, L"HANDSHAKE_TEST");

    if (!WriteFile(gConsultantPipe, &handshakeRequest, sizeof(handshakeRequest), &bytesWritten, NULL)) {
        wprintf(L"  [Handshake] Failed to send request (error %lu)\n", GetLastError());
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    wprintf(L"  [Handshake] Request sent (%lu bytes), waiting for response...\n", bytesWritten);

    //
    //  Read handshake response
    //

    if (!ReadFile(gConsultantPipe, &handshakeResponse, sizeof(handshakeResponse), &bytesRead, NULL)) {
        wprintf(L"  [Handshake] Failed to read response (error %lu)\n", GetLastError());
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    wprintf(L"  [Handshake] Response received (%lu bytes)\n", bytesRead);

    //
    //  Verify handshake response
    //

    if (bytesRead < sizeof(handshakeResponse)) {
        wprintf(L"  [Handshake] Response too small (%lu < %zu)\n", bytesRead, sizeof(handshakeResponse));
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    if (handshakeResponse.Version != AVF_CONSULTANT_PROTOCOL_VERSION) {
        wprintf(L"  [Handshake] Version mismatch (got %lu, expected %d)\n", 
                handshakeResponse.Version, AVF_CONSULTANT_PROTOCOL_VERSION);
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    if (handshakeResponse.RequestId != 0) {
        wprintf(L"  [Handshake] RequestId mismatch (got %lu, expected 0)\n", handshakeResponse.RequestId);
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    wprintf(L"  [Handshake] SUCCESS - Consultant ready (Decision=%lu, Reason=%lu)\n",
            handshakeResponse.Decision, handshakeResponse.Reason);

    gConsultantConnected = TRUE;
    return TRUE;
}


BOOL
QueryConsultant(
    _In_ PAVF_FILE_NOTIFICATION pNotification,
    _Out_ PAVF_CONSULTANT_RESPONSE pResponse
    )
/*++

Routine Description:

    Sends a file access query to the security consultant and gets the response.

Arguments:

    pNotification - File access notification to query.
    pResponse - Receives the consultant's response.

Return Value:

    TRUE if query succeeded, FALSE otherwise.

--*/
{
    AVF_CONSULTANT_REQUEST request;
    DWORD bytesWritten;
    DWORD bytesRead;
    static ULONG requestId = 0;

    if (!gConsultantConnected || gConsultantPipe == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    //
    //  Build request
    //

    RtlZeroMemory(&request, sizeof(request));
    request.Version = AVF_CONSULTANT_PROTOCOL_VERSION;
    request.RequestId = ++requestId;
    request.ProcessId = pNotification->ProcessId;
    request.Operation = pNotification->MajorFunction;
    wcscpy_s(request.FileName, AVF_MAX_PATH, pNotification->FileName);
    wcscpy_s(request.ProcessName, 260, pNotification->ProcessName);

    //
    //  Send request
    //

    if (!WriteFile(gConsultantPipe, &request, sizeof(request), &bytesWritten, NULL)) {
        //
        //  Pipe broken - consultant disconnected
        //
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    //
    //  Read response
    //

    if (!ReadFile(gConsultantPipe, pResponse, sizeof(*pResponse), &bytesRead, NULL)) {
        //
        //  Pipe broken - consultant disconnected
        //
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
        gConsultantConnected = FALSE;
        return FALSE;
    }

    if (bytesRead < sizeof(*pResponse)) {
        return FALSE;
    }

    //
    //  Verify response matches request
    //

    if (pResponse->Version != AVF_CONSULTANT_PROTOCOL_VERSION ||
        pResponse->RequestId != request.RequestId) {
        return FALSE;
    }

    return TRUE;
}
