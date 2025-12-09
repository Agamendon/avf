# AVF Security Consultant Protocol

## Overview

The AVF (AV Filter) user-mode process can delegate security decisions to an external **Security Consultant** process. This allows easy swapping of security logic without modifying the core AVF components.

## Communication Mechanism

**Transport:** Windows Named Pipe  
**Pipe Name:** `\\.\pipe\AvfSecurityConsultant`  
**Mode:** Message mode (synchronous request/response)  
**Timeout:** 15 seconds (configurable via `AVF_CONSULTANT_TIMEOUT_MS`)

## Protocol Flow

```
???????????????         ????????????????????         ???????????????????????
?   Kernel    ?         ?   AVF User App   ?         ? Security Consultant ?
?  Minifilter ?         ?    (avf.exe)     ?         ?                     ?
???????????????         ????????????????????         ???????????????????????
       ?                         ?                              ?
       ?  File Access Event      ?                              ?
       ?????????????????????????>?                              ?
       ?                         ?                              ?
       ?                         ?  AVF_CONSULTANT_REQUEST      ?
       ?                         ??????????????????????????????>?
       ?                         ?                              ?
       ?                         ?                              ? (evaluate)
       ?                         ?                              ?
       ?                         ?  AVF_CONSULTANT_RESPONSE     ?
       ?                         ?<??????????????????????????????
       ?                         ?                              ?
       ?  Allow/Block Reply      ?                              ?
       ?<?????????????????????????                              ?
       ?                         ?                              ?
```

## Startup Sequence

1. **Security Consultant starts first** and creates the named pipe server
2. AVF user app starts and attempts to connect to the pipe
3. If connection fails, AVF operates in "allow all" mode
4. AVF can be restarted to reconnect to a newly started consultant

## Data Structures

### Request (AVF ? Consultant)

```c
typedef struct _AVF_CONSULTANT_REQUEST {
    ULONG Version;                     // Protocol version (currently 1)
    ULONG RequestId;                   // Unique request ID for correlation
    ULONG ProcessId;                   // PID of process accessing the file
    ULONG Operation;                   // IRP_MJ_CREATE (0), IRP_MJ_READ (3), or IRP_MJ_WRITE (4)
    WCHAR ProcessName[260];            // Name of the accessing process
    WCHAR FileName[520];               // Full NT path of the file
} AVF_CONSULTANT_REQUEST;
```

**Total Size:** ~1576 bytes

### Response (Consultant ? AVF)

```c
typedef struct _AVF_CONSULTANT_RESPONSE {
    ULONG Version;                     // Must match request version
    ULONG RequestId;                   // Must match request ID
    ULONG Decision;                    // 0 = ALLOW, 1 = BLOCK
    ULONG Reason;                      // Consultant-defined reason code
} AVF_CONSULTANT_RESPONSE;
```

**Total Size:** 16 bytes

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `AVF_CONSULTANT_PROTOCOL_VERSION` | 1 | Current protocol version |
| `AVF_DECISION_ALLOW` | 0 | Allow the file operation |
| `AVF_DECISION_BLOCK` | 1 | Block the file operation |
| `IRP_MJ_CREATE` | 0 | File open/create operation |
| `IRP_MJ_READ` | 3 | File read operation |
| `IRP_MJ_WRITE` | 4 | File write operation |

## Implementing a Security Consultant

### Minimal Implementation (C/C++)

```c
#include <windows.h>
#include "avf.h"

int main() {
    HANDLE hPipe;
    AVF_CONSULTANT_REQUEST request;
    AVF_CONSULTANT_RESPONSE response;
    DWORD bytesRead, bytesWritten;

    // Create named pipe server
    hPipe = CreateNamedPipeW(
        AVF_CONSULTANT_PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,                          // Max instances
        sizeof(AVF_CONSULTANT_RESPONSE),
        sizeof(AVF_CONSULTANT_REQUEST),
        0,
        NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        return 1;
    }

    printf("Security Consultant waiting for connection...\n");

    // Wait for client connection
    if (!ConnectNamedPipe(hPipe, NULL)) {
        CloseHandle(hPipe);
        return 1;
    }

    printf("AVF connected. Processing requests...\n");

    // Main loop
    while (TRUE) {
        // Read request
        if (!ReadFile(hPipe, &request, sizeof(request), &bytesRead, NULL)) {
            break;  // Client disconnected
        }

        // Validate version
        if (request.Version != AVF_CONSULTANT_PROTOCOL_VERSION) {
            continue;
        }

        //
        // ??? YOUR SECURITY LOGIC HERE ???
        //
        // Example: Block all writes to .exe files
        //
        BOOL shouldBlock = FALSE;
        if (request.Operation == IRP_MJ_WRITE) {
            if (wcsstr(request.FileName, L".EXE") != NULL) {
                shouldBlock = TRUE;
            }
        }

        // Build response
        response.Version = AVF_CONSULTANT_PROTOCOL_VERSION;
        response.RequestId = request.RequestId;
        response.Decision = shouldBlock ? AVF_DECISION_BLOCK : AVF_DECISION_ALLOW;
        response.Reason = shouldBlock ? 1 : 0;  // Your reason code

        // Send response
        if (!WriteFile(hPipe, &response, sizeof(response), &bytesWritten, NULL)) {
            break;  // Client disconnected
        }
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    return 0;
}
```

### Python Implementation

```python
import signal
import struct
import time
import win32pipe
import win32file
import pywintypes

PIPE_NAME = r'\\.\pipe\AvfSecurityConsultant'
PROTOCOL_VERSION = 1
DECISION_ALLOW = 0
DECISION_BLOCK = 1

# Structure sizes (must match avf.h)
REQUEST_SIZE = 4 + 4 + 4 + 4 + 520 + 1040  # 1576 bytes
RESPONSE_SIZE = 16  # 4 ULONGs
RESPONSE_FORMAT = '<IIII'

g_running = True

def signal_handler(sig, frame):
    global g_running
    print("\nShutting down...")
    g_running = False

def create_pipe():
    return win32pipe.CreateNamedPipe(
        PIPE_NAME,
        win32pipe.PIPE_ACCESS_DUPLEX,
        win32pipe.PIPE_TYPE_MESSAGE | win32pipe.PIPE_READMODE_MESSAGE | win32pipe.PIPE_NOWAIT,
        win32pipe.PIPE_UNLIMITED_INSTANCES,
        RESPONSE_SIZE,
        REQUEST_SIZE,
        0,
        None
    )

def parse_request(data):
    if len(data) < REQUEST_SIZE:
        return None
    
    version, request_id, process_id, operation = struct.unpack('<IIII', data[:16])
    process_name = data[16:536].decode('utf-16-le', errors='ignore').split('\x00')[0]
    file_name = data[536:1576].decode('utf-16-le', errors='ignore').split('\x00')[0]
    
    return {
        'version': version,
        'request_id': request_id,
        'process_id': process_id,
        'operation': operation,
        'process_name': process_name,
        'file_name': file_name
    }

def make_response(request_id, decision, reason=0):
    return struct.pack(RESPONSE_FORMAT, PROTOCOL_VERSION, request_id, decision, reason)

def should_block(request):
    """
    YOUR SECURITY LOGIC HERE - Keep it FAST!
    """
    # Handle handshake request (Operation = 0xFF)
    if request['operation'] == 0xFF:
        print(f"  [Handshake] Received from PID {request['process_id']}")
        return False  # Always allow handshake
    
    # Example: Block ALL writes, allow reads and opens
    if request['operation'] == 4:  # IRP_MJ_WRITE
        return True
    # Uncomment to also block file opens:
    # if request['operation'] == 0:  # IRP_MJ_CREATE
    #     return True
    return False

def get_operation_name(op):
    """Convert operation code to string."""
    ops = {0: 'OPEN', 3: 'READ', 4: 'WRITE', 0xFF: 'HSHAKE'}
    return ops.get(op, f'OP{op}')

def handle_client(pipe):
    """Handle a single client connection."""
    # Switch to blocking mode for reads
    win32pipe.SetNamedPipeHandleState(pipe, win32pipe.PIPE_READMODE_MESSAGE | win32pipe.PIPE_WAIT, None, None)
    
    request_count = 0
    try:
        while g_running:
            try:
                result, data = win32file.ReadFile(pipe, REQUEST_SIZE)
                request_count += 1
                
                request = parse_request(data)
                if request is None:
                    win32file.WriteFile(pipe, make_response(0, DECISION_ALLOW))
                    continue
                
                decision = DECISION_BLOCK if should_block(request) else DECISION_ALLOW
                win32file.WriteFile(pipe, make_response(request['request_id'], decision))
                
                op = get_operation_name(request['operation'])
                dec = 'BLOCK' if decision else 'ALLOW'
                print(f"[{request_count:4d}] [{dec:5s}] [{op:5s}] PID:{request['process_id']:5d} {request['process_name']} -> {request['file_name']}")
                
            except pywintypes.error as e:
                if e.winerror in (109, 232, 995):  # BROKEN_PIPE, NO_DATA, ABORTED
                    break
                raise
    except KeyboardInterrupt:
        pass
    
    return request_count

def main():
    global g_running
    
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGBREAK, signal_handler)
    
    print(f"Security Consultant listening on {PIPE_NAME}")
    print("Start avf.exe at any time - connection is automatic.")
    print("Press Ctrl+C to exit.\n")
    
    total_requests = 0
    session_count = 0
    
    while g_running:
        pipe = None
        try:
            pipe = create_pipe()
            print("Waiting for AVF to connect...")
            
            # Poll for connection (non-blocking pipe)
            connected = False
            while g_running and not connected:
                try:
                    win32pipe.ConnectNamedPipe(pipe, None)
                    connected = True
                except pywintypes.error as e:
                    if e.winerror == 535:  # ERROR_PIPE_CONNECTED - already connected!
                        connected = True
                    elif e.winerror == 536:  # ERROR_PIPE_LISTENING - no client yet
                        time.sleep(0.1)  # Poll every 100ms
                    else:
                        raise
            
            if not g_running:
                break
            
            session_count += 1
            print(f"[Session {session_count}] AVF connected!\n")
            
            requests = handle_client(pipe)
            total_requests += requests
            
            print(f"[Session {session_count}] Ended. {requests} requests.\n")
            
        except pywintypes.error as e:
            if e.winerror == 231:  # ERROR_PIPE_BUSY
                time.sleep(0.1)
                continue
            elif e.winerror != 995:  # Not ABORTED
                print(f"Error: {e}")
        except KeyboardInterrupt:
            break
        finally:
            if pipe:
                try:
                    win32pipe.DisconnectNamedPipe(pipe)
                except:
                    pass
                try:
                    win32file.CloseHandle(pipe)
                except:
                    pass
    
    print(f"\nTotal: {session_count} sessions, {total_requests} requests. Goodbye!")

if __name__ == '__main__':
    main()
```

> **IMPORTANT**: The kernel driver waits up to 15 seconds for a response. Long-running analysis is supported, but will block the file operation during that time.

> **Note**: Start avf.exe or the consultant in any order - they auto-connect on file access.

## Error Handling

| Scenario | AVF Behavior |
|----------|--------------|
| Consultant not running | Allow all operations, log warning |
| Consultant disconnects | Allow all operations, log warning |
| Response timeout | Allow operation, continue |
| Version mismatch | Ignore response, allow operation |
| RequestId mismatch | Ignore response, allow operation |

## Reason Codes

The `Reason` field in the response is consultant-defined. Suggested codes:

| Code | Meaning |
|------|---------|
| 0 | No specific reason (allowed) |
| 1 | Blocked by policy |
| 2 | Suspicious process |
| 3 | Protected file |
| 4 | Write to system file |
| 5+ | Custom/application-specific |

## File Path Format

The `FileName` field contains the **NT device path**, not the Win32 path:

- **NT Path:** `\Device\HarddiskVolume4\Users\User\Desktop\file.txt`
- **Win32 Path:** `C:\Users\User\Desktop\file.txt`

To convert NT path to Win32 path in your consultant, you can:
1. Enumerate volumes with `FindFirstVolume`/`FindNextVolume`
2. Map device names using `QueryDosDevice`
3. Build a lookup table at startup

## Security Considerations

1. **Run consultant with appropriate privileges** - It needs to respond quickly
2. **Fail-open design** - If consultant fails, operations are allowed
3. **Timeout handling** - Long-running evaluations will timeout
4. **Single instance** - Only one AVF can connect at a time
5. **No authentication** - Any process can connect to the pipe; add ACLs if needed

## Testing

1. Start your security consultant first
2. Start AVF: `avf.exe C:\path\to\protected\file.txt`
3. Access the protected file from another process
4. Observe consultant decisions in both AVF and consultant output

dir deploy\avf.sys
   dir deploy\avf.exe
