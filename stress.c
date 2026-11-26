/*
  Stress test for Windows virtual memory behaviour, modelled after what
  mariadbd does with the InnoDB buffer pool: reserve a huge address range,
  commit/decommit chunks inside it, and meanwhile do ordinary malloc/free.

  Parent mode: spawns N children, samples system counters, reports failures.
  Child mode : does the work; on the first failed allocation dumps the state
               of the system and of the process, and exits with nonzero code.

  Like mariadbd, each child reserves --reserve-gb, commits --chunk-mb at the
  start of it once, touches it, and holds it for the whole run.

  Usage: stress [--procs N] [--reserve-gb G] [--chunk-mb M] [--seconds S]
                [--churn]
         --reserve-gb 0 : no big reservation (baseline, plain commit)
         --churn        : additionally commit/decommit a second chunk
                          repeatedly (like innodb_buffer_pool_size resizing)
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096
#define SLOTS 256

static unsigned long long rng_state;
static unsigned long long rnd(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

static void sys_line(const char *tag)
{
  PERFORMANCE_INFORMATION pi= {sizeof pi};
  MEMORYSTATUSEX ms= {sizeof ms};
  if (!GetPerformanceInfo(&pi, sizeof pi) || !GlobalMemoryStatusEx(&ms))
    return;
  printf("%s commit=%llu/%lluMB kpaged=%lluMB knonpaged=%lluMB "
         "availphys=%lluMB procs=%lu\n", tag,
         (unsigned long long) pi.CommitTotal * pi.PageSize >> 20,
         (unsigned long long) pi.CommitLimit * pi.PageSize >> 20,
         (unsigned long long) pi.KernelPaged * pi.PageSize >> 20,
         (unsigned long long) pi.KernelNonpaged * pi.PageSize >> 20,
         (unsigned long long) ms.ullAvailPhys >> 20, pi.ProcessCount);
  fflush(stdout);
}

/* Free address space of this process: total and largest free block */
static void va_summary(unsigned long long *total_free,
                       unsigned long long *largest_free)
{
  SYSTEM_INFO si;
  char *p;
  MEMORY_BASIC_INFORMATION mbi;
  GetSystemInfo(&si);
  *total_free= *largest_free= 0;
  for (p= (char *) si.lpMinimumApplicationAddress;
       p < (char *) si.lpMaximumApplicationAddress &&
       VirtualQuery(p, &mbi, sizeof mbi);
       p= (char *) mbi.BaseAddress + mbi.RegionSize)
  {
    if (mbi.State == MEM_FREE)
    {
      *total_free+= mbi.RegionSize;
      if (mbi.RegionSize > *largest_free)
        *largest_free= mbi.RegionSize;
    }
  }
}

static void dump_failure(const char *what, int id, DWORD err)
{
  PROCESS_MEMORY_COUNTERS_EX pmc= {sizeof pmc};
  unsigned long long total, largest;
  char tag[64];
  _snprintf(tag, sizeof tag, "FAIL[%d] %s (err %lu) system:", id, what,
            (unsigned long) err);
  sys_line(tag);
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *) &pmc,
                       sizeof pmc);
  va_summary(&total, &largest);
  printf("FAIL[%d] process: private=%lluMB pagefileusage=%lluMB "
         "pagedpoolquota=%lluKB nonpagedpoolquota=%lluKB "
         "freeVA=%lluGB largestfreeVA=%lluGB\n", id,
         (unsigned long long) pmc.PrivateUsage >> 20,
         (unsigned long long) pmc.PagefileUsage >> 20,
         (unsigned long long) pmc.QuotaPagedPoolUsage >> 10,
         (unsigned long long) pmc.QuotaNonPagedPoolUsage >> 10,
         total >> 30, largest >> 30);
  fflush(stdout);
}

static int child(int id, size_t reserve_gb, size_t chunk_mb, int seconds,
                 int churn)
{
  size_t reserve= reserve_gb << 30, chunk= chunk_mb << 20;
  char *base= NULL;
  void *slot[SLOTS]= {0};
  ULONGLONG end;
  unsigned long long iter= 0;

  rng_state= 0x9E3779B97F4A7C15ULL ^ ((unsigned long long) GetCurrentProcessId() << 20) ^ id;

  if (reserve)
  {
    base= VirtualAlloc(NULL, reserve, MEM_RESERVE, PAGE_READWRITE);
    if (!base)
    {
      dump_failure("reserve", id, GetLastError());
      return 3;
    }
  }

  /* the "buffer pool": committed once at the start, held until exit */
  {
    char *pool;
    size_t n;
    if (base)
      pool= VirtualAlloc(base, chunk, MEM_COMMIT, PAGE_READWRITE);
    else
      pool= VirtualAlloc(NULL, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pool)
    {
      dump_failure("pool commit", id, GetLastError());
      return 4;
    }
    for (n= 0; n < chunk; n+= PAGE)
      pool[n]= 1;
  }

  end= GetTickCount64() + (ULONGLONG) seconds * 1000;
  while (GetTickCount64() < end)
  {
    int k;
    /* ordinary heap churn, 16 bytes .. 1 MB */
    for (k= 0; k < 64; k++)
    {
      size_t i= rnd() % SLOTS, size= (size_t) 16 << (rnd() % 17);
      char *p;
      free(slot[i]);
      slot[i]= NULL;
      p= malloc(size);
      if (!p)
      {
        dump_failure("malloc", id, GetLastError());
        return 2;
      }
      memset(p, 1, size < PAGE ? size : PAGE);
      slot[i]= p;
    }

    /* optional resize-like churn on a second chunk */
    if (churn && iter++ % 8 == 0)
    {
      char *p;
      size_t n;
      if (base)
        p= VirtualAlloc(base + chunk, chunk, MEM_COMMIT, PAGE_READWRITE);
      else
        p= VirtualAlloc(NULL, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (!p)
      {
        dump_failure("churn commit", id, GetLastError());
        return 5;
      }
      for (n= 0; n < chunk; n+= PAGE)
        p[n]= 1;
      if (base)
        VirtualFree(p, chunk, MEM_DECOMMIT);
      else
        VirtualFree(p, 0, MEM_RELEASE);
    }
    else if (!churn)
      iter++;
  }
  printf("child %d finished OK, %llu iterations\n", id, iter);
  return 0;
}

int main(int argc, char **argv)
{
  int procs= 8, seconds= 60, churn= 0, i, failed= 0, child_id= -1;
  size_t reserve_gb= 8192, chunk_mb= 128;

  for (i= 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--procs") && i + 1 < argc) procs= atoi(argv[++i]);
    else if (!strcmp(argv[i], "--reserve-gb") && i + 1 < argc)
      reserve_gb= (size_t) _atoi64(argv[++i]);
    else if (!strcmp(argv[i], "--chunk-mb") && i + 1 < argc)
      chunk_mb= (size_t) _atoi64(argv[++i]);
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds= atoi(argv[++i]);
    else if (!strcmp(argv[i], "--churn")) churn= 1;
    else if (!strcmp(argv[i], "--child") && i + 1 < argc) child_id= atoi(argv[++i]);
    else
    {
      fprintf(stderr, "bad argument: %s\n", argv[i]);
      return 64;
    }
  }

  if (child_id >= 0)
    return child(child_id, reserve_gb, chunk_mb, seconds, churn);

  /* parent */
  {
    char exe[MAX_PATH], cmd[MAX_PATH + 200];
    HANDLE h[MAXIMUM_WAIT_OBJECTS];
    int running;
    ULONGLONG t0= GetTickCount64();
    if (procs > MAXIMUM_WAIT_OBJECTS)
      procs= MAXIMUM_WAIT_OBJECTS;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    printf("procs=%d reserve=%zuGB chunk=%zuMB seconds=%d churn=%d\n",
           procs, reserve_gb, chunk_mb, seconds, churn);
    sys_line("start:");
    for (i= 0; i < procs; i++)
    {
      STARTUPINFOA si= {sizeof si};
      PROCESS_INFORMATION pi;
      _snprintf(cmd, sizeof cmd,
                "\"%s\" --child %d --reserve-gb %zu --chunk-mb %zu "
                "--seconds %d%s", exe, i, reserve_gb, chunk_mb, seconds,
                churn ? " --churn" : "");
      if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
      {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 65;
      }
      CloseHandle(pi.hThread);
      h[i]= pi.hProcess;
    }
    running= procs;
    while (running)
    {
      DWORD r= WaitForMultipleObjects(procs, h, FALSE, 10000);
      if (r == WAIT_TIMEOUT)
      {
        char tag[32];
        _snprintf(tag, sizeof tag, "t=%llus:", (GetTickCount64() - t0) / 1000);
        sys_line(tag);
        continue;
      }
      if (r < (DWORD) procs)
      {
        DWORD code;
        GetExitCodeProcess(h[r], &code);
        if (code)
          failed++;
        CloseHandle(h[r]);
        h[r]= h[--running];
        procs--;
      }
      else
        break;
    }
    sys_line("end:");
    printf("%s: %d child process(es) failed\n", failed ? "FAILED" : "OK", failed);
    return failed ? 1 : 0;
  }
}
