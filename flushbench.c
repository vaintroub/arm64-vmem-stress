/*
  Does NtFlushBuffersFileEx(FLUSH_FLAGS_*_ONLY) do anything compared to
  FlushFileBuffers()?  Overwrites 4 KiB blocks in a preallocated file
  (no size change, like InnoDB data files) and flushes after every write.

  Usage: flushbench <directory> [seconds-per-variant]
  Prints the filesystem, NTSTATUS of each flavour, and flushes/second.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLUSH_FLAGS_FILE_DATA_ONLY 0x1
#define FLUSH_FLAGS_NO_SYNC 0x2
#define FLUSH_FLAGS_FILE_DATA_SYNC_ONLY 0x4

typedef NTSTATUS(WINAPI *flush_ex_t)(HANDLE, ULONG, PVOID, ULONG,
                                     PIO_STATUS_BLOCK);
static flush_ex_t flush_ex;

#define FILE_SIZE (64u << 20)
#define BLK 4096

static int do_flush(HANDLE h, int variant, NTSTATUS *st)
{
  IO_STATUS_BLOCK iosb;
  ULONG flags;
  memset(&iosb, 0, sizeof iosb);
  switch (variant)
  {
  case 0: return FlushFileBuffers(h) ? 0 : -1;
  case 1: flags= FLUSH_FLAGS_FILE_DATA_SYNC_ONLY; break;
  case 2: flags= FLUSH_FLAGS_FILE_DATA_ONLY; break;
  default: return 0; /* 3: no flush at all, upper bound */
  }
  *st= flush_ex(h, flags, NULL, 0, &iosb);
  return *st ? -1 : 0;
}

static const char *names[]= {"FlushFileBuffers", "Ex DATA_SYNC_ONLY",
                             "Ex DATA_ONLY", "(no flush)"};

int main(int argc, char **argv)
{
  char path[MAX_PATH], root[4]= "X:\\", fsname[64]= "?";
  int seconds= argc > 2 ? atoi(argv[2]) : 3, variant, nobuf;
  void *buf;
  if (argc < 2)
  {
    fprintf(stderr, "usage: flushbench <dir> [seconds]\n");
    return 64;
  }
  flush_ex= (flush_ex_t) GetProcAddress(GetModuleHandleA("ntdll"),
                                        "NtFlushBuffersFileEx");
  _snprintf(path, sizeof path, "%s\\flushbench.tmp", argv[1]);
  root[0]= path[0];
  GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fsname, sizeof fsname);
  printf("dir=%s fs=%s NtFlushBuffersFileEx=%s\n", argv[1], fsname,
         flush_ex ? "present" : "MISSING");
  buf= _aligned_malloc(BLK, BLK);
  memset(buf, 0xAB, BLK);

  for (nobuf= 0; nobuf < 2; nobuf++)
  {
    printf("-- %s writes\n", nobuf ? "NO_BUFFERING" : "buffered");
    for (variant= 0; variant < 4; variant++)
    {
      NTSTATUS st= 0;
      HANDLE h= CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL |
                            (nobuf ? FILE_FLAG_NO_BUFFERING : 0), NULL);
      ULONGLONG end;
      unsigned long long n= 0, failed= 0;
      DWORD wr, off;
      LARGE_INTEGER sz;
      if (h == INVALID_HANDLE_VALUE)
      {
        printf("CreateFile failed: %lu\n", GetLastError());
        return 1;
      }
      /* preallocate by writing, so that overwrites change no metadata */
      sz.QuadPart= FILE_SIZE;
      SetFilePointerEx(h, sz, NULL, FILE_BEGIN);
      SetEndOfFile(h);
      for (off= 0; off < FILE_SIZE; off+= BLK)
      {
        LARGE_INTEGER p;
        p.QuadPart= off;
        SetFilePointerEx(h, p, NULL, FILE_BEGIN);
        WriteFile(h, buf, BLK, &wr, NULL);
      }
      FlushFileBuffers(h);

      end= GetTickCount64() + (ULONGLONG) seconds * 1000;
      off= 0;
      while (GetTickCount64() < end)
      {
        LARGE_INTEGER p;
        p.QuadPart= off;
        SetFilePointerEx(h, p, NULL, FILE_BEGIN);
        WriteFile(h, buf, BLK, &wr, NULL);
        if (variant != 0 && variant != 3 && !flush_ex)
          break;
        if (do_flush(h, variant, &st))
          failed++;
        n++;
        off= (off + BLK) % FILE_SIZE;
      }
      printf("  %-18s %9llu flushes/s   failed=%llu lastNTSTATUS=0x%08lX\n",
             names[variant], n / seconds, failed, (unsigned long) st);
      CloseHandle(h);
    }
  }
  DeleteFileA(path);
  return 0;
}
