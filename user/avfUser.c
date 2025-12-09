/*++

Copyright (c) 1989-2002  Microsoft Corporation
Modified for AV Filter functionality

Module Name:

    avfUser.c

Abstract:

    This file contains the main function for the user-mode component
    of the AV Filter. It connects to the kernel minifilter and receives
    notifications about file accesses.

    This version uses multiple worker threads to handle requests concurrently,
    preventing deadlocks when the consultant needs to access files.

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
//  Configuration
//

#define AVF_WORKER_THREAD_COUNT     4
#define AVF_MAX_PENDING_REQUESTS    16

//
//  Global variables
//

HANDLE gPort = INVALID_HANDLE_VALUE;
HANDLE gCompletionPort = INVALID_HANDLE_VALUE;
volatile BOOLEAN gRunning = TRUE;

//
//  Consultant connection - protected by critical section
//

CRITICAL_SECTION gConsultantLock;
HANDLE gConsultantPipe = INVALID_HANDLE_VALUE;
BOOLEAN gConsultantConnected = FALSE;

//
//  Protected files list - stores NT device paths for comparison
//

#define MAX_PROTECTED_FILES 100
WCHAR gProtectedFiles[MAX_PROTECTED_FILES][AVF_MAX_PATH];
ULONG gProtectedFileCount = 0;

//
//  Worker thread context
//

typedef struct _AVF_WORKER_CONTEXT {
    HANDLE Port;
    HANDLE CompletionPort;
} AVF_WORKER_CONTEXT, *PAVF_WORKER_CONTEXT;

//
//  Message structure for async operations
//

typedef struct _AVF_MESSAGE {
    FILTER_MESSAGE_HEADER Header;
    AVF_FILE_NOTIFICATION Notification;
    OVERLAPPED Overlapped;
} AVF_MESSAGE, *PAVF_MESSAGE;

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

BOOL WINAPI
ConsoleCtrlHandler(
    DWORD CtrlType
    );

DWORD WINAPI
WorkerThread(
    _In_ LPVOID lpParameter
    );


int
wmain(
    _In_ int argc,
    _In_reads_(argc) WCHAR *argv[]
    )
/*++

Routine Description:

    Main entry point for the userspace listener application.
    Creates worker threads and an I/O completion port for concurrent request handling.

Arguments:

    argc - Argument count.
    argv - Argument vector.

Return Value:

    Exit code.

--*/
{
    HRESULT hr;
    int i;
    HANDLE workerThreads[AVF_WORKER_THREAD_COUNT];
    AVF_WORKER_CONTEXT workerContext;
    PAVF_MESSAGE messages[AVF_MAX_PENDING_REQUESTS];
    DWORD threadId;

    wprintf(L"AV Filter - File Access Monitor (Multi-threaded)\n");
    wprintf(L"=================================================\n\n");

    //
    //  Initialize critical section for consultant access
    //

    InitializeCriticalSection(&gConsultantLock);

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
        DeleteCriticalSection(&gConsultantLock);
        return 1;
    }

    wprintf(L"Connected to avf filter.\n");

    //
    //  Create I/O completion port
    //

    gCompletionPort = CreateIoCompletionPort(gPort, NULL, 0, AVF_WORKER_THREAD_COUNT);
    if (gCompletionPort == NULL) {
        wprintf(L"ERROR: Failed to create completion port (error %lu)\n", GetLastError());
        CloseHandle(gPort);
        DeleteCriticalSection(&gConsultantLock);
        return 1;
    }

    //
    //  Try to connect to security consultant
    //

    if (ConnectToConsultant()) {
        wprintf(L"Connected to security consultant.\n");
    } else {
        wprintf(L"Security consultant not available - will allow all operations.\n");
        wprintf(L"Start consultant to enable security decisions.\n");
    }

    wprintf(L"\nStarting %d worker threads...\n", AVF_WORKER_THREAD_COUNT);

    //
    //  Set up worker context
    //

    workerContext.Port = gPort;
    workerContext.CompletionPort = gCompletionPort;

    //
    //  Create worker threads
    //

    for (i = 0; i < AVF_WORKER_THREAD_COUNT; i++) {
        workerThreads[i] = CreateThread(
                            NULL,
                            0,
                            WorkerThread,
                            &workerContext,
                            0,
                            &threadId);

        if (workerThreads[i] == NULL) {
            wprintf(L"ERROR: Failed to create worker thread %d (error %lu)\n", i, GetLastError());
        } else {
            wprintf(L"  Worker thread %d started (TID %lu)\n", i, threadId);
        }
    }

    //
    //  Allocate message structures and queue initial async reads
    //

    for (i = 0; i < AVF_MAX_PENDING_REQUESTS; i++) {
        messages[i] = (PAVF_MESSAGE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AVF_MESSAGE));
        if (messages[i] != NULL) {
            hr = FilterGetMessage(gPort, &messages[i]->Header, sizeof(AVF_MESSAGE), &messages[i]->Overlapped);
            if (hr != HRESULT_FROM_WIN32(ERROR_IO_PENDING) && FAILED(hr)) {
                wprintf(L"WARNING: Failed to queue message %d (0x%08X)\n", i, hr);
            }
        }
    }

    wprintf(L"\nWaiting for file access events...\n\n");

    //
    //  Wait for shutdown signal
    //

    while (gRunning) {
        Sleep(100);
    }

    //
    //  Signal workers to stop by posting completion packets
    //

    for (i = 0; i < AVF_WORKER_THREAD_COUNT; i++) {
        PostQueuedCompletionStatus(gCompletionPort, 0, 0, NULL);
    }

    //
    //  Wait for worker threads to exit
    //

    WaitForMultipleObjects(AVF_WORKER_THREAD_COUNT, workerThreads, TRUE, 5000);

    //
    //  Cleanup worker threads
    //

    for (i = 0; i < AVF_WORKER_THREAD_COUNT; i++) {
        if (workerThreads[i] != NULL) {
            CloseHandle(workerThreads[i]);
        }
    }

    //
    //  Free message structures
    //

    for (i = 0; i < AVF_MAX_PENDING_REQUESTS; i++) {
        if (messages[i] != NULL) {
            HeapFree(GetProcessHeap(), 0, messages[i]);
        }
    }

    //
    //  Cleanup
    //

    EnterCriticalSection(&gConsultantLock);
    if (gConsultantPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(gConsultantPipe);
        gConsultantPipe = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&gConsultantLock);

    if (gCompletionPort != INVALID_HANDLE_VALUE) {
        CloseHandle(gCompletionPort);
        gCompletionPort = INVALID_HANDLE_VALUE;
    }

    if (gPort != INVALID_HANDLE_VALUE) {
        CloseHandle(gPort);
        gPort = INVALID_HANDLE_VALUE;
    }

    DeleteCriticalSection(&gConsultantLock);

    wprintf(L"\nExiting...\n");
    return 0;
}


DWORD WINAPI
WorkerThread(
    _In_ LPVOID lpParameter
    )
/*++

Routine Description:

    Worker thread that processes file access notifications from the kernel.

Arguments:

    lpParameter - Pointer to AVF_WORKER_CONTEXT.

Return Value:

    Thread exit code.

--*/
{
    PAVF_WORKER_CONTEXT context = (PAVF_WORKER_CONTEXT)lpParameter;
    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    LPOVERLAPPED overlapped;
    PAVF_MESSAGE message;
    PAVF_FILE_NOTIFICATION pNotification;
    HRESULT hr;
    DWORD threadId = GetCurrentThreadId();

    struct {
        FILTER_REPLY_HEADER Header;
        AVF_REPLY Reply;
    } replyBuffer;

    while (gRunning) {
        //
        //  Wait for a completion packet
        //

        if (!GetQueuedCompletionStatus(
                context->CompletionPort,
                &bytesTransferred,
                &completionKey,
                &overlapped,
                1000)) {  // 1 second timeout

            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            if (error == ERROR_OPERATION_ABORTED) {
                continue;
            }
            break;
        }

        //
        //  Check for shutdown signal (NULL overlapped)
        //

        if (overlapped == NULL) {
            break;
        }

        //
        //  Get the message structure from the overlapped pointer
        //

        message = CONTAINING_RECORD(overlapped, AVF_MESSAGE, Overlapped);
        pNotification = &message->Notification;

        //
        //  Check if this file is in our protected list
        //

        if (gProtectedFileCount == 0 || IsFileProtected(pNotification->FileName)) {

            //
            //  Print the file access information
            //

            wprintf(L"[T%lu] [%s] PID: %5lu  Process: %-20s  File: %s\n",
                    threadId,
                    pNotification->MajorFunction == IRP_MJ_CREATE ? L"OPEN " :
                    pNotification->MajorFunction == IRP_MJ_READ ? L"READ " : L"WRITE",
                    pNotification->ProcessId,
                    pNotification->ProcessName,
                    pNotification->FileName);

            //
            //  Query security consultant (thread-safe)
            //

            EnterCriticalSection(&gConsultantLock);

            if (!gConsultantConnected) {
                //
                //  Try to reconnect to consultant
                //
                if (ConnectToConsultant()) {
                    wprintf(L"  [T%lu] -> Connected to security consultant\n", threadId);
                }
            }

            if (gConsultantConnected) {
                AVF_CONSULTANT_RESPONSE response;

                LeaveCriticalSection(&gConsultantLock);

                //
                //  Query consultant outside of lock to allow concurrent queries
                //

                EnterCriticalSection(&gConsultantLock);
                BOOL connected = gConsultantConnected;
                LeaveCriticalSection(&gConsultantLock);

                if (connected && QueryConsultant(pNotification, &response)) {
                    if (response.Decision == AVF_DECISION_BLOCK) {
                        wprintf(L"  [T%lu] -> BLOCKED by consultant (reason code: %lu)\n", threadId, response.Reason);
                        replyBuffer.Reply.BlockOperation = 1;
                    } else {
                        wprintf(L"  [T%lu] -> ALLOWED by consultant\n", threadId);
                        replyBuffer.Reply.BlockOperation = 0;
                    }
                } else {
                    wprintf(L"  [T%lu] -> Consultant disconnected, allowing\n", threadId);
                    replyBuffer.Reply.BlockOperation = 0;
                }
            } else {
                LeaveCriticalSection(&gConsultantLock);
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
        replyBuffer.Header.MessageId = message->Header.MessageId;

        hr = FilterReplyMessage(
                context->Port,
                &replyBuffer.Header,
                sizeof(replyBuffer.Header) + sizeof(replyBuffer.Reply));

        if (FAILED(hr)) {
            wprintf(L"  [T%lu] WARNING: FilterReplyMessage failed (0x%08X)\n", threadId, hr);
        }

        //
        //  Queue another async read using the same message buffer
        //

        RtlZeroMemory(&message->Overlapped, sizeof(OVERLAPPED));
        hr = FilterGetMessage(context->Port, &message->Header, sizeof(AVF_MESSAGE), &message->Overlapped);
        if (hr != HRESULT_FROM_WIN32(ERROR_IO_PENDING) && FAILED(hr)) {
            if (hr != HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) {
                wprintf(L"  [T%lu] WARNING: FilterGetMessage failed (0x%08X)\n", threadId, hr);
            }
        }
    }

    wprintf(L"  [T%lu] Worker thread exiting\n", threadId);
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
    Thread-safe - uses critical section to protect pipe access.

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
    static volatile LONG requestId = 0;
    ULONG thisRequestId;

    EnterCriticalSection(&gConsultantLock);

    if (!gConsultantConnected || gConsultantPipe == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&gConsultantLock);
        return FALSE;
    }

    //
    //  Build request
    //

    thisRequestId = (ULONG)InterlockedIncrement(&requestId);

    RtlZeroMemory(&request, sizeof(request));
    request.Version = AVF_CONSULTANT_PROTOCOL_VERSION;
    request.RequestId = thisRequestId;
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
        LeaveCriticalSection(&gConsultantLock);
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
        LeaveCriticalSection(&gConsultantLock);
        return FALSE;
    }

    LeaveCriticalSection(&gConsultantLock);

    if (bytesRead < sizeof(*pResponse)) {
        return FALSE;
    }

    //
    //  Verify response matches request
    //

    if (pResponse->Version != AVF_CONSULTANT_PROTOCOL_VERSION ||
        pResponse->RequestId != thisRequestId) {
        return FALSE;
    }

    return TRUE;
}
