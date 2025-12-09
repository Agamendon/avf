/*++

Copyright (c) 1989-2002  Microsoft Corporation
Modified for AV Filter functionality

Module Name:

    avf.c

Abstract:

    This is the main module of the AVF mini-filter driver.
    It intercepts file read/write operations and notifies the
    userspace listener about file access to protected files.

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

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

//
//  Global variables
//

PFLT_FILTER gFilterHandle = NULL;
PFLT_PORT gServerPort = NULL;
PFLT_PORT gClientPort = NULL;

//
//  Pool tags
//

#define AVF_POOL_TAG 'FvAM'

//
//  Function prototypes
//

DRIVER_INITIALIZE DriverEntry;
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    );

NTSTATUS
AvfUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    );

NTSTATUS
AvfInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    );

NTSTATUS
AvfInstanceQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    );

FLT_PREOP_CALLBACK_STATUS
AvfPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

FLT_PREOP_CALLBACK_STATUS
AvfPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

FLT_PREOP_CALLBACK_STATUS
AvfPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

NTSTATUS
AvfPortConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID *ConnectionCookie
    );

VOID
AvfPortDisconnect(
    _In_opt_ PVOID ConnectionCookie
    );

NTSTATUS
AvfMessageNotify(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    );

//
//  Operation registration
//

CONST FLT_OPERATION_REGISTRATION Callbacks[] = {

    { IRP_MJ_CREATE,
      0,
      AvfPreCreate,
      NULL },

    { IRP_MJ_READ,
      0,
      AvfPreRead,
      NULL },

    { IRP_MJ_WRITE,
      0,
      AvfPreWrite,
      NULL },

    { IRP_MJ_OPERATION_END }
};

//
//  Context definitions
//

CONST FLT_CONTEXT_REGISTRATION ContextRegistration[] = {

    { FLT_CONTEXT_END }
};

//
//  Filter registration structure
//

CONST FLT_REGISTRATION FilterRegistration = {

    sizeof(FLT_REGISTRATION),           //  Size
    FLT_REGISTRATION_VERSION,           //  Version
    0,                                  //  Flags
    ContextRegistration,                //  Context
    Callbacks,                          //  Operation callbacks
    AvfUnload,                          //  FilterUnload
    AvfInstanceSetup,                   //  InstanceSetup
    AvfInstanceQueryTeardown,           //  InstanceQueryTeardown
    NULL,                               //  InstanceTeardownStart
    NULL,                               //  InstanceTeardownComplete
    NULL,                               //  GenerateFileName
    NULL,                               //  GenerateDestinationFileName
    NULL                                //  NormalizeNameComponent
};


//
//  Implementation
//

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    This is the initialization routine for this AVF mini-filter driver.

Arguments:

    DriverObject - Pointer to driver object created by the system.
    RegistryPath - Unicode string identifying where the parameters for this
                   driver are located in the registry.

Return Value:

    Returns STATUS_SUCCESS.

--*/
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING portName;
    PSECURITY_DESCRIPTOR sd;

    UNREFERENCED_PARAMETER(RegistryPath);

    //
    //  Register with FltMgr
    //

    status = FltRegisterFilter(DriverObject,
                               &FilterRegistration,
                               &gFilterHandle);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    //  Create communication port
    //

    RtlInitUnicodeString(&portName, AVF_PORT_NAME);

    status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);

    if (NT_SUCCESS(status)) {

        InitializeObjectAttributes(&oa,
                                   &portName,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL,
                                   sd);

        status = FltCreateCommunicationPort(gFilterHandle,
                                            &gServerPort,
                                            &oa,
                                            NULL,
                                            AvfPortConnect,
                                            AvfPortDisconnect,
                                            AvfMessageNotify,
                                            1);

        FltFreeSecurityDescriptor(sd);

        if (!NT_SUCCESS(status)) {
            FltUnregisterFilter(gFilterHandle);
            return status;
        }
    }

    //
    //  Start filtering
    //

    status = FltStartFiltering(gFilterHandle);

    if (!NT_SUCCESS(status)) {
        FltCloseCommunicationPort(gServerPort);
        FltUnregisterFilter(gFilterHandle);
        return status;
    }

    DbgPrint("AVF: Driver loaded successfully\n");
    return STATUS_SUCCESS;
}


NTSTATUS
AvfUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
/*++

Routine Description:

    This is the unload routine for this AVF mini-filter driver.

Arguments:

    Flags - Indicating if this is a mandatory unload.

Return Value:

    Returns STATUS_SUCCESS.

--*/
{
    UNREFERENCED_PARAMETER(Flags);

    if (gServerPort != NULL) {
        FltCloseCommunicationPort(gServerPort);
    }

    if (gFilterHandle != NULL) {
        FltUnregisterFilter(gFilterHandle);
    }

    DbgPrint("AVF: Driver unloaded\n");
    return STATUS_SUCCESS;
}


NTSTATUS
AvfInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    )
/*++

Routine Description:

    This routine is called whenever a new instance is created on a volume.

Arguments:

    FltObjects - Pointer to the FLT_RELATED_OBJECTS structure.
    Flags - Flags describing the reason for this attach request.
    VolumeDeviceType - Device type of the file system volume.
    VolumeFilesystemType - File system type of the volume.

Return Value:

    STATUS_SUCCESS - attach
    STATUS_FLT_DO_NOT_ATTACH - do not attach

--*/
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeFilesystemType);

    //
    //  Don't attach to network volumes
    //

    if (VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM) {
        return STATUS_FLT_DO_NOT_ATTACH;
    }

    return STATUS_SUCCESS;
}


NTSTATUS
AvfInstanceQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    )
/*++

Routine Description:

    This is called when an instance is being manually deleted.

Arguments:

    FltObjects - Pointer to the FLT_RELATED_OBJECTS structure.
    Flags - Flags describing the reason for this teardown request.

Return Value:

    STATUS_SUCCESS

--*/
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);

    return STATUS_SUCCESS;
}


BOOLEAN
AvfSendNotification(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ UCHAR MajorFunction
    )
/*++

Routine Description:

    Sends a file access notification to the user-mode listener.

Arguments:

    Data - Pointer to the filter callbackData.
    FltObjects - Pointer to the FLT_RELATED_OBJECTS structure.
    MajorFunction - IRP major function code (read/write).

Return Value:

    TRUE if operation should be blocked, FALSE to allow.

--*/
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    AVF_FILE_NOTIFICATION notification;
    AVF_REPLY reply;
    PEPROCESS process;
    LARGE_INTEGER timeout;
    ULONG replyLength;

    UNREFERENCED_PARAMETER(FltObjects);

    //
    //  Check if we have a client connected
    //

    if (gClientPort == NULL) {
        return FALSE;  // No client, allow operation
    }

    //
    //  Get the file name
    //

    status = FltGetFileNameInformation(Data,
                                       FLT_FILE_NAME_NORMALIZED |
                                       FLT_FILE_NAME_QUERY_DEFAULT,
                                       &nameInfo);

    if (!NT_SUCCESS(status)) {
        return FALSE;  // Can't get name, allow operation
    }

    status = FltParseFileNameInformation(nameInfo);

    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(nameInfo);
        return FALSE;  // Can't parse name, allow operation
    }

    //
    //  Initialize notification structure
    //

    RtlZeroMemory(&notification, sizeof(notification));
    RtlZeroMemory(&reply, sizeof(reply));

    notification.ProcessId = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    notification.MajorFunction = MajorFunction;

    //
    //  Copy file name
    //

    if (nameInfo->Name.Length < sizeof(notification.FileName) - sizeof(WCHAR)) {
        RtlCopyMemory(notification.FileName,
                      nameInfo->Name.Buffer,
                      nameInfo->Name.Length);
        notification.FileName[nameInfo->Name.Length / sizeof(WCHAR)] = L'\0';
    }

    FltReleaseFileNameInformation(nameInfo);

    //
    //  Get process name
    //

    process = PsGetCurrentProcess();
    if (process != NULL) {
        PUCHAR imageName = PsGetProcessImageFileName(process);
        if (imageName != NULL) {
            //
            //  Convert ANSI process name to Unicode
            //
            ANSI_STRING ansiName;
            UNICODE_STRING unicodeName;

            RtlInitAnsiString(&ansiName, (PCSZ)imageName);
            unicodeName.Buffer = notification.ProcessName;
            unicodeName.MaximumLength = sizeof(notification.ProcessName) - sizeof(WCHAR);
            unicodeName.Length = 0;

            RtlAnsiStringToUnicodeString(&unicodeName, &ansiName, FALSE);
        }
    }

    //
    //  Send notification to user mode and wait for reply
    //

    replyLength = sizeof(reply);
    timeout.QuadPart = -600000000LL;  // 60 second timeout (100ns units, negative = relative)

    status = FltSendMessage(gFilterHandle,
                            &gClientPort,
                            &notification,
                            sizeof(notification),
                            &reply,
                            &replyLength,
                            &timeout);

    if (NT_SUCCESS(status)) {
        //
        //  Got a reply - check if we should block
        //
        if (reply.BlockOperation != 0) {
            DbgPrint("AVF: Blocking operation on %wZ\n", &notification.FileName);
            return TRUE;  // Block the operation
        }
    } else {
        //
        //  Failed to send or timeout - allow operation by default
        //
        if (status == STATUS_TIMEOUT) {
            DbgPrint("AVF: Timeout waiting for user response\n");
        } else if (status != STATUS_PORT_DISCONNECTED) {
            DbgPrint("AVF: Failed to send notification, status=0x%x\n", status);
        }
    }

    return FALSE;  // Allow operation
}


FLT_PREOP_CALLBACK_STATUS
AvfPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
/*++

Routine Description:

    Pre-create callback. Notifies userspace about file open/create access.

Arguments:

    Data - Pointer to the filter callbackData.
    FltObjects - Pointer to the FLT_RELATED_OBJECTS structure.
    CompletionContext - Pointer to completion context.

Return Value:

    FLT_PREOP_SUCCESS_NO_CALLBACK or FLT_PREOP_COMPLETE if blocked.

--*/
{
    BOOLEAN shouldBlock;

    UNREFERENCED_PARAMETER(CompletionContext);

    //
    //  Skip kernel mode requests
    //

    if (Data->RequestorMode == KernelMode) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    //  Skip directory opens (we only care about files)
    //

    if (FlagOn(Data->Iopb->Parameters.Create.Options, FILE_DIRECTORY_FILE)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    //  Send notification to userspace and check if we should block
    //

    shouldBlock = AvfSendNotification(Data, FltObjects, IRP_MJ_CREATE);

    if (shouldBlock) {
        //
        //  Block the operation - return ACCESS_DENIED
        //
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}


FLT_PREOP_CALLBACK_STATUS
AvfPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
/*++

Routine Description:

    Pre-read callback. Notifies userspace about file read access.

Arguments:

    Data - Pointer to the filter callbackData.
    FltObjects - Pointer to the FLT_RELATED_OBJECTS structure.
    CompletionContext - Pointer to completion context.

Return Value:

    FLT_PREOP_SUCCESS_NO_CALLBACK or FLT_PREOP_COMPLETE if blocked.

--*/
{
    BOOLEAN shouldBlock;

    UNREFERENCED_PARAMETER(CompletionContext);

    //
    //  Skip kernel mode requests
    //

    if (Data->RequestorMode == KernelMode) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    //  Skip paging I/O
    //

    if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    //  Send notification to userspace and check if we should block
    //

    shouldBlock = AvfSendNotification(Data, FltObjects, IRP_MJ_READ);

    if (shouldBlock) {
        //
        //  Block the operation - return ACCESS_DENIED
        //
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}


FLT_PREOP_CALLBACK_STATUS
AvfPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
/*++

Routine Description:

    Pre-write callback. Notifies userspace about file write access.

Arguments:

    Data - Pointer to the filter callbackData.
    FltObjects - Pointer to the FLT_RELATED_OBJECTS structure.
    CompletionContext - Pointer to completion context.

Return Value:

    FLT_PREOP_SUCCESS_NO_CALLBACK or FLT_PREOP_COMPLETE if blocked.

--*/
{
    BOOLEAN shouldBlock;

    UNREFERENCED_PARAMETER(CompletionContext);

    //
    //  Skip kernel mode requests
    //

    if (Data->RequestorMode == KernelMode) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    //  Skip paging I/O
    //

    if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    //  Send notification to userspace and check if we should block
    //

    shouldBlock = AvfSendNotification(Data, FltObjects, IRP_MJ_WRITE);

    if (shouldBlock) {
        //
        //  Block the operation - return ACCESS_DENIED
        //
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}


NTSTATUS
AvfPortConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID *ConnectionCookie
    )
/*++

Routine Description:

    Called when a user-mode application connects to the communication port.

Arguments:

    ClientPort - Client port that will be used to send messages.
    ServerPortCookie - Not used.
    ConnectionContext - Context data from the connection request.
    SizeOfContext - Size of the context data.
    ConnectionCookie - Returned connection cookie.

Return Value:

    STATUS_SUCCESS

--*/
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    gClientPort = ClientPort;
    *ConnectionCookie = NULL;

    DbgPrint("AVF: Client connected\n");
    return STATUS_SUCCESS;
}


VOID
AvfPortDisconnect(
    _In_opt_ PVOID ConnectionCookie
    )
/*++

Routine Description:

    Called when a user-mode application disconnects from the communication port.

Arguments:

    ConnectionCookie - Connection cookie (not used).

Return Value:

    None

--*/
{
    UNREFERENCED_PARAMETER(ConnectionCookie);

    FltCloseClientPort(gFilterHandle, &gClientPort);
    gClientPort = NULL;

    DbgPrint("AVF: Client disconnected\n");
}


NTSTATUS
AvfMessageNotify(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    )
/*++

Routine Description:

    Handles messages sent from user mode to the filter.

Arguments:

    PortCookie - Not used.
    InputBuffer - Input buffer from user mode.
    InputBufferLength - Length of input buffer.
    OutputBuffer - Output buffer to user mode.
    OutputBufferLength - Length of output buffer.
    ReturnOutputBufferLength - Actual length of data returned.

Return Value:

    STATUS_SUCCESS

--*/
{
    PCOMMAND_MESSAGE command;

    UNREFERENCED_PARAMETER(PortCookie);

    *ReturnOutputBufferLength = 0;

    if (InputBuffer == NULL || InputBufferLength < sizeof(COMMAND_MESSAGE)) {
        return STATUS_INVALID_PARAMETER;
    }

    command = (PCOMMAND_MESSAGE)InputBuffer;

    switch (command->Command) {

    case GetAvfVersion:
        if (OutputBuffer != NULL && OutputBufferLength >= sizeof(AVFVER)) {
            PAVFVER version = (PAVFVER)OutputBuffer;
            version->Major = AVF_MAJ_VERSION;
            version->Minor = AVF_MIN_VERSION;
            *ReturnOutputBufferLength = sizeof(AVFVER);
        }
        break;

        default:
            break;
    }

    return STATUS_SUCCESS;
}
