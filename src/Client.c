#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "Api.h"

#define WMIGUID_EXECUTE 0x0010
#define WMI_METHOD_ID 1U
#define REQUEST_SIZE 4096U
#define RESPONSE_SIZE 512U
#define RESPONSE_DATA_SIZE 352U
#define REQ_MAGIC 0x99D4B1698CAF040FULL
#define RESP_MAGIC 0xC0D8E93F90BBFBECULL
#define STATUS_OK 0U

#define PING 1U
#define READ_PHYS 2U
#define WRITE_PHYS 3U
#define TRANSLATE_VIRT 4U
#define READ_VIRT 5U
#define WRITE_VIRT 6U
#define FIND_PROCESS_PID 7U
#define FIND_PROCESS_NAME 8U
#define FIND_MODULE 9U
#define FIND_KERNEL_MODULE 10U
#define FIND_EXPORT 11U

typedef ULONG(WINAPI *WMI_OPEN_BLOCK)(GUID *Guid, DWORD DesiredAccess,
                                      HANDLE *DataBlockHandle);
typedef ULONG(WINAPI *WMI_EXECUTE_METHOD_W)(HANDLE DataBlockHandle,
                                            const wchar_t *InstanceName,
                                            ULONG MethodId,
                                            ULONG InBufferSize,
                                            void *InBuffer,
                                            ULONG *OutBufferSize,
                                            void *OutBuffer);
typedef ULONG(WINAPI *WMI_CLOSE_BLOCK)(HANDLE DataBlockHandle);

#pragma pack(push, 1)
typedef struct {
  uint64_t Magic;
  uint32_t Command;
  uint32_t DataSize;
  uint64_t Sequence;
  uint64_t Arg1;
  uint64_t Arg2;
  uint64_t Arg3;
  uint8_t Data[1];
} REQUEST;

typedef struct {
  uint64_t Magic;
  uint32_t Status;
  uint32_t Command;
  uint32_t DataSize;
  uint64_t Sequence;
  uint64_t Result;
  uint8_t Data[RESPONSE_DATA_SIZE];
} RESPONSE;

#pragma pack(pop)

typedef struct {
  HMODULE Advapi;
  WMI_OPEN_BLOCK OpenBlock;
  WMI_EXECUTE_METHOD_W ExecuteMethod;
  WMI_CLOSE_BLOCK CloseBlock;
  HANDLE Block;
  const wchar_t **Instances;
  uint64_t Sequence;
} STATE;

static GUID gMemGuid = {
    0xa0c9f8de,
    0x0b71,
    0x42a8,
    {0xb9, 0x67, 0xe5, 0x38, 0xea, 0xcb, 0x6f, 0x21}};
static const wchar_t *gMemInstances[] = {
    L"ACPI\\PNP0C14\\Mem_0",
    L"ACPI\\PNP0C14\\SMMM_0",
    L"ACPI\\PNP0C14\\0_0",
    L"",
    NULL};

static STATE g;

static void CloseWmi(void) {
  if (g.CloseBlock != NULL && g.Block != NULL) {
    g.CloseBlock(g.Block);
  }
  if (g.Advapi != NULL) {
    FreeLibrary(g.Advapi);
  }
  ZeroMemory(&g, sizeof(g));
}

static int OpenWmi(GUID *Guid, const wchar_t **Instances) {
  ULONG Status;

  CloseWmi();
  g.Advapi = LoadLibraryW(L"Advapi32.dll");
  if (g.Advapi == NULL) {
    return 0;
  }
  g.OpenBlock = (WMI_OPEN_BLOCK)GetProcAddress(g.Advapi, "WmiOpenBlock");
  g.ExecuteMethod =
      (WMI_EXECUTE_METHOD_W)GetProcAddress(g.Advapi, "WmiExecuteMethodW");
  g.CloseBlock = (WMI_CLOSE_BLOCK)GetProcAddress(g.Advapi, "WmiCloseBlock");
  if (g.OpenBlock == NULL || g.ExecuteMethod == NULL ||
      g.CloseBlock == NULL) {
    CloseWmi();
    return 0;
  }
  Status = g.OpenBlock(Guid, WMIGUID_EXECUTE, &g.Block);
  if (Status != ERROR_SUCCESS) {
    CloseWmi();
    return 0;
  }
  g.Instances = Instances;
  g.Sequence = GetTickCount64();
  return 1;
}

int Init(void) {
  return OpenWmi(&gMemGuid, gMemInstances);
}

void Close(void) {
  CloseWmi();
}

static void InitRequest(REQUEST *Request, uint32_t Command) {
  ZeroMemory(Request, REQUEST_SIZE);
  Request->Magic = REQ_MAGIC;
  Request->Command = Command;
  Request->Sequence = ++g.Sequence;
}

static int ExecuteWmi(REQUEST *Request, RESPONSE *Response) {
  uint8_t Out[RESPONSE_SIZE];
  ULONG OutSize;
  ULONG Status;

  for (size_t Index = 0; g.Instances[Index] != NULL; Index++) {
    ZeroMemory(Out, sizeof(Out));
    OutSize = sizeof(Out);
    Status = g.ExecuteMethod(g.Block, g.Instances[Index], WMI_METHOD_ID,
                             REQUEST_SIZE, Request, &OutSize, Out);
    if (Status == ERROR_SUCCESS) {
      CopyMemory(Response, Out, sizeof(*Response));
      return Response->Magic == RESP_MAGIC;
    }
  }
  return 0;
}

static int Send(REQUEST *Request, RESPONSE *Response) {
  if (g.Block == NULL) {
    return 0;
  }
  ZeroMemory(Response, sizeof(*Response));
  return ExecuteWmi(Request, Response);
}

int Ping(void) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;

  InitRequest(Request, PING);
  return Send(Request, &Response) && Response.Status == STATUS_OK;
}

int FindProcessByPid(uint32_t Pid, PROCESS_INFO *Process) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;

  InitRequest(Request, FIND_PROCESS_PID);
  Request->Arg1 = Pid;
  if (!Send(Request, &Response) || Response.Status != STATUS_OK ||
      Response.DataSize < sizeof(*Process)) {
    return 0;
  }
  CopyMemory(Process, Response.Data, sizeof(*Process));
  return 1;
}

int FindProcessByName(const char *Name, PROCESS_INFO *Process) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  size_t Size = strlen(Name) + 1;

  if (Size > RESPONSE_DATA_SIZE) {
    return 0;
  }
  InitRequest(Request, FIND_PROCESS_NAME);
  Request->DataSize = (uint32_t)Size;
  CopyMemory(Request->Data, Name, Size);
  if (!Send(Request, &Response) || Response.Status != STATUS_OK ||
      Response.DataSize < sizeof(*Process)) {
    return 0;
  }
  CopyMemory(Process, Response.Data, sizeof(*Process));
  return 1;
}

int TranslateVirt(uint32_t Pid, uint64_t Va, uint64_t *Pa) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;

  InitRequest(Request, TRANSLATE_VIRT);
  Request->Arg1 = Pid;
  Request->Arg2 = Va;
  if (!Send(Request, &Response) || Response.Status != STATUS_OK) {
    return 0;
  }
  *Pa = Response.Result;
  return 1;
}

int ReadPhys(uint64_t Address, void *Buffer, uint32_t Size) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  uint32_t Done = 0;

  while (Done < Size) {
    uint32_t Chunk = Size - Done;
    if (Chunk > RESPONSE_DATA_SIZE) {
      Chunk = RESPONSE_DATA_SIZE;
    }
    InitRequest(Request, READ_PHYS);
    Request->Arg1 = Address + Done;
    Request->Arg2 = Chunk;
    if (!Send(Request, &Response) || Response.Status != STATUS_OK ||
        Response.DataSize != Chunk) {
      return 0;
    }
    CopyMemory((uint8_t *)Buffer + Done, Response.Data, Chunk);
    Done += Chunk;
  }
  return 1;
}

int WritePhys(uint64_t Address, const void *Buffer, uint32_t Size) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  uint32_t Done = 0;

  while (Done < Size) {
    uint32_t Chunk = Size - Done;
    if (Chunk > RESPONSE_DATA_SIZE) {
      Chunk = RESPONSE_DATA_SIZE;
    }
    InitRequest(Request, WRITE_PHYS);
    Request->Arg1 = Address + Done;
    Request->DataSize = Chunk;
    CopyMemory(Request->Data, (const uint8_t *)Buffer + Done, Chunk);
    if (!Send(Request, &Response) || Response.Status != STATUS_OK) {
      return 0;
    }
    Done += Chunk;
  }
  return 1;
}

int ReadVirt(uint32_t Pid, uint64_t Address, void *Buffer, uint32_t Size) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  uint32_t Done = 0;

  while (Done < Size) {
    uint32_t Chunk = Size - Done;
    if (Chunk > RESPONSE_DATA_SIZE) {
      Chunk = RESPONSE_DATA_SIZE;
    }
    InitRequest(Request, READ_VIRT);
    Request->Arg1 = Pid;
    Request->Arg2 = Address + Done;
    Request->Arg3 = Chunk;
    if (!Send(Request, &Response) || Response.Status != STATUS_OK ||
        Response.DataSize != Chunk) {
      return 0;
    }
    CopyMemory((uint8_t *)Buffer + Done, Response.Data, Chunk);
    Done += Chunk;
  }
  return 1;
}

int WriteVirt(uint32_t Pid, uint64_t Address, const void *Buffer,
              uint32_t Size) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  uint32_t Done = 0;

  while (Done < Size) {
    uint32_t Chunk = Size - Done;
    if (Chunk > RESPONSE_DATA_SIZE) {
      Chunk = RESPONSE_DATA_SIZE;
    }
    InitRequest(Request, WRITE_VIRT);
    Request->Arg1 = Pid;
    Request->Arg2 = Address + Done;
    Request->DataSize = Chunk;
    CopyMemory(Request->Data, (const uint8_t *)Buffer + Done, Chunk);
    if (!Send(Request, &Response) || Response.Status != STATUS_OK) {
      return 0;
    }
    Done += Chunk;
  }
  return 1;
}

int FindModule(const PROCESS_INFO *Process, const char *Name,
               MODULE_INFO *Module) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  size_t Size = strlen(Name) + 1;

  if (Size > RESPONSE_DATA_SIZE) {
    return 0;
  }
  InitRequest(Request, FIND_MODULE);
  Request->Arg1 = Process->Pid;
  Request->DataSize = (uint32_t)Size;
  CopyMemory(Request->Data, Name, Size);
  if (!Send(Request, &Response) || Response.Status != STATUS_OK ||
      Response.DataSize < sizeof(*Module)) {
    return 0;
  }
  CopyMemory(Module, Response.Data, sizeof(*Module));
  return 1;
}

int FindKernelModule(const char *Name, MODULE_INFO *Module) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  size_t Size = strlen(Name) + 1;

  if (Size > RESPONSE_DATA_SIZE) {
    return 0;
  }
  InitRequest(Request, FIND_KERNEL_MODULE);
  Request->DataSize = (uint32_t)Size;
  CopyMemory(Request->Data, Name, Size);
  if (!Send(Request, &Response) || Response.Status != STATUS_OK ||
      Response.DataSize < sizeof(*Module)) {
    return 0;
  }
  CopyMemory(Module, Response.Data, sizeof(*Module));
  return 1;
}

int FindExport(const MODULE_INFO *Module, const char *Name,
               uint64_t *Address) {
  uint8_t In[REQUEST_SIZE];
  REQUEST *Request = (REQUEST *)In;
  RESPONSE Response;
  size_t NameSize = strlen(Name) + 1;
  uint32_t Size = (uint32_t)(sizeof(*Module) + NameSize);

  if (Size > RESPONSE_DATA_SIZE) {
    return 0;
  }
  InitRequest(Request, FIND_EXPORT);
  Request->DataSize = Size;
  CopyMemory(Request->Data, Module, sizeof(*Module));
  CopyMemory(Request->Data + sizeof(*Module), Name, NameSize);
  if (!Send(Request, &Response) || Response.Status != STATUS_OK) {
    return 0;
  }
  *Address = Response.Result;
  return 1;
}

int Dump(const MODULE_INFO *Module, DUMP_CALLBACK Callback, void *Context) {
  uint8_t Buffer[RESPONSE_DATA_SIZE];
  uint64_t Done = 0;

  if (Module == NULL || Module->Pid == 0 || Module->Base == 0 ||
      Module->Size == 0) {
    return 0;
  }
  while (Done < Module->Size) {
    uint64_t Address = Module->Base + Done;
    uint32_t Chunk = (uint32_t)(Module->Size - Done);
    if (Chunk > sizeof(Buffer)) {
      Chunk = sizeof(Buffer);
    }
    if (Chunk > 0x1000U - (uint32_t)(Address & 0xFFFU)) {
      Chunk = 0x1000U - (uint32_t)(Address & 0xFFFU);
    }
    if (!ReadVirt(Module->Pid, Address, Buffer, Chunk)) {
      ZeroMemory(Buffer, Chunk);
    }
    if (Callback != NULL && !Callback(Address, Buffer, Chunk, Context)) {
      return 0;
    }
    Done += Chunk;
  }
  return 1;
}

#ifndef API_ONLY
static double ElapsedSec(LARGE_INTEGER Start, LARGE_INTEGER End,
                         LARGE_INTEGER Freq) {
  return (double)(End.QuadPart - Start.QuadPart) / (double)Freq.QuadPart;
}

static void PrintThroughput(const char *Label, uint64_t Bytes, double Sec,
                            int Ok) {
  double Mb = (double)Bytes / (1024.0 * 1024.0);
  double Mbps = Sec > 0.0 ? Mb / Sec : 0.0;
  double Ms = Sec * 1000.0;
  printf("%-20s %s  %7.2f MB  %8.2f ms  %8.2f MB/s\n", Label,
         Ok ? "OK  " : "FAIL", Mb, Ms, Mbps);
}

/* ReadPhys chunks at RESPONSE_DATA_SIZE (352). Keep transferring until
   TargetBytes is reached, walking a fixed physical window. */
static int ReadPhysBandwidth(uint64_t PhysBase, uint64_t TargetBytes,
                             uint32_t ChunkSize, uint8_t *Buffer,
                             uint64_t *OutBytes) {
  uint64_t Done = 0;
  uint64_t Offset = 0;
  const uint64_t Window = 0x100000ULL; /* 1 MB window starting at PhysBase */

  if (ChunkSize == 0 || ChunkSize > RESPONSE_DATA_SIZE) {
    ChunkSize = RESPONSE_DATA_SIZE;
  }
  while (Done < TargetBytes) {
    uint32_t Chunk = ChunkSize;
    if ((uint64_t)Chunk > TargetBytes - Done) {
      Chunk = (uint32_t)(TargetBytes - Done);
    }
    if (!ReadPhys(PhysBase + Offset, Buffer, Chunk)) {
      *OutBytes = Done;
      return 0;
    }
    Done += Chunk;
    Offset += Chunk;
    if (Offset + ChunkSize > Window) {
      Offset = 0;
    }
  }
  *OutBytes = Done;
  return 1;
}

static int RunSpeedtest(void) {
  LARGE_INTEGER Freq;
  LARGE_INTEGER Start;
  LARGE_INTEGER End;
  uint8_t Buffer[RESPONSE_DATA_SIZE];
  uint64_t Bytes = 0;
  uint32_t PhysVal = 0;
  int Ok;
  int Pass = 1;
  double Sec;
  const uint64_t Sizes[] = {
      64ULL * 1024ULL,       /* 64 KB */
      256ULL * 1024ULL,      /* 256 KB */
      1ULL * 1024ULL * 1024ULL, /* 1 MB */
      4ULL * 1024ULL * 1024ULL, /* 4 MB */
  };
  size_t Index;

  QueryPerformanceFrequency(&Freq);

  printf("=== ReadPhys speedtest ===\n");
  printf("chunk size: %u bytes (WMI/SMM response payload)\n\n",
         RESPONSE_DATA_SIZE);

  Ok = Init();
  if (!Ok) {
    printf("Init failed\n");
    return 0;
  }

  QueryPerformanceCounter(&Start);
  Ok = Ping();
  QueryPerformanceCounter(&End);
  Sec = ElapsedSec(Start, End, Freq);
  printf("%-20s %s  %8.3f ms\n", "Ping", Ok ? "OK  " : "FAIL", Sec * 1000.0);
  if (!Ok) {
    Close();
    return 0;
  }

  QueryPerformanceCounter(&Start);
  Ok = ReadPhys(0x1000, &PhysVal, sizeof(PhysVal));
  QueryPerformanceCounter(&End);
  Sec = ElapsedSec(Start, End, Freq);
  printf("%-20s %s  %8.3f ms  value=0x%08X\n", "ReadPhys 4B <4GB",
         Ok ? "OK  " : "FAIL", Sec * 1000.0, PhysVal);
  Pass &= Ok;

  QueryPerformanceCounter(&Start);
  Ok = ReadPhys(0x200000000ULL, &PhysVal, sizeof(PhysVal));
  QueryPerformanceCounter(&End);
  Sec = ElapsedSec(Start, End, Freq);
  printf("%-20s %s  %8.3f ms  value=0x%08X\n", "ReadPhys 4B >4GB",
         Ok ? "OK  " : "FAIL", Sec * 1000.0, PhysVal);

  /* warmup so first timed run is not cold SMM */
  ReadPhysBandwidth(0x1000, 64ULL * 1024ULL, RESPONSE_DATA_SIZE, Buffer,
                    &Bytes);

  printf("\n%-20s %-4s  %10s  %10s  %12s\n", "test", "stat", "data", "time",
         "throughput");
  printf("------------------------------------------------------------\n");

  for (Index = 0; Index < sizeof(Sizes) / sizeof(Sizes[0]); Index++) {
    QueryPerformanceCounter(&Start);
    Ok = ReadPhysBandwidth(0x1000, Sizes[Index], RESPONSE_DATA_SIZE, Buffer,
                           &Bytes);
    QueryPerformanceCounter(&End);
    Sec = ElapsedSec(Start, End, Freq);
    PrintThroughput("ReadPhys", Bytes, Sec, Ok);
    Pass &= Ok;
    if (!Ok) {
      printf("  stopped after %llu bytes\n", (unsigned long long)Bytes);
      break;
    }
  }

  /* single max-chunk latency / implied rate */
  {
    const int Rounds = 100;
    int I;
    Ok = 1;
    QueryPerformanceCounter(&Start);
    for (I = 0; I < Rounds; I++) {
      if (!ReadPhys(0x1000, Buffer, RESPONSE_DATA_SIZE)) {
        Ok = 0;
        break;
      }
    }
    QueryPerformanceCounter(&End);
    Sec = ElapsedSec(Start, End, Freq);
    Bytes = Ok ? (uint64_t)Rounds * RESPONSE_DATA_SIZE : 0;
    PrintThroughput("ReadPhys 352B x100", Bytes, Sec, Ok);
    if (Ok && Sec > 0.0) {
      printf("  per call: %.3f ms  (%.0f calls/s)\n",
             (Sec * 1000.0) / (double)Rounds, (double)Rounds / Sec);
    }
    Pass &= Ok;
  }

  /*
   * Process / kernel lookups temporarily disabled until firmware update.
   *
   * PROCESS_INFO Process;
   * MODULE_INFO Module;
   * uint64_t Address = 0;
   * FindProcessByPid(4, &Process);
   * FindProcessByName("notepad.exe", &Process);
   * FindKernelModule("ntoskrnl.exe", &Module);
   * FindExport(&Module, "PsInitialSystemProcess", &Address);
   * ReadVirt(...);
   */

  printf("\n=== done (%s) ===\n", Pass ? "pass" : "fail");
  Close();
  return Pass;
}

int wmain(int argc, wchar_t **argv) {
  if (argc == 1 ||
      (argc == 2 && (_wcsicmp(argv[1], L"ping") == 0 ||
                     _wcsicmp(argv[1], L"speedtest") == 0))) {
    return RunSpeedtest() ? 0 : 1;
  }
  printf("Usage: Client.exe [ping|speedtest]\n");
  return 1;
}
#endif