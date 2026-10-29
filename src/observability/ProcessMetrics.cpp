#include "observability/ProcessMetrics.h"

#include "observability/Metrics.h"

#if defined(_WIN32)
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#endif

#include <cstdint>

namespace urlshort {

#if defined(_WIN32)
namespace {

std::uint64_t fileTimeTo100ns(const FILETIME& value) {
  ULARGE_INTEGER combined;
  combined.LowPart = value.dwLowDateTime;
  combined.HighPart = value.dwHighDateTime;
  return combined.QuadPart;
}

std::int64_t processThreadCount() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }

  const DWORD current_process = GetCurrentProcessId();
  std::int64_t count = 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID == current_process) {
        ++count;
      }
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return count;
}

}  // namespace

void sampleProcessMetrics(Metrics& metrics) {
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  std::uint64_t cpu_ms = 0;
  if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
    const std::uint64_t total_100ns = fileTimeTo100ns(kernel) + fileTimeTo100ns(user);
    cpu_ms = total_100ns / 10'000;
  }

  std::uint64_t rss_bytes = 0;
  PROCESS_MEMORY_COUNTERS_EX memory{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                           sizeof(memory))) {
    rss_bytes = static_cast<std::uint64_t>(memory.WorkingSetSize);
  }

  metrics.setProcess(cpu_ms, rss_bytes, processThreadCount());
}
#elif defined(__linux__)
void sampleProcessMetrics(Metrics& metrics) {
  std::uint64_t cpu_ms = 0;
  std::uint64_t rss_bytes = 0;
  std::int64_t threads = 0;

  std::ifstream stat("/proc/self/stat");
  std::string line;
  if (std::getline(stat, line)) {
    const auto comm_end = line.rfind(')');
    if (comm_end != std::string::npos && comm_end + 2 < line.size()) {
      std::istringstream fields(line.substr(comm_end + 2));
      std::string state;
      long long ppid = 0;
      long long pgrp = 0;
      long long session = 0;
      long long tty_nr = 0;
      long long tpgid = 0;
      unsigned long long flags = 0;
      unsigned long long minflt = 0;
      unsigned long long cminflt = 0;
      unsigned long long majflt = 0;
      unsigned long long cmajflt = 0;
      unsigned long long utime = 0;
      unsigned long long stime = 0;
      long long cutime = 0;
      long long cstime = 0;
      long long priority = 0;
      long long nice = 0;
      long long thread_count = 0;
      long long itrealvalue = 0;
      unsigned long long starttime = 0;
      unsigned long long vsize = 0;
      long long rss_pages = 0;

      if (fields >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags >> minflt >>
              cminflt >> majflt >> cmajflt >> utime >> stime >> cutime >> cstime >> priority >>
              nice >> thread_count >> itrealvalue >> starttime >> vsize >> rss_pages) {
        const long ticks_per_second = sysconf(_SC_CLK_TCK);
        if (ticks_per_second > 0) {
          cpu_ms = (utime + stime) * 1000ULL /
                   static_cast<unsigned long long>(ticks_per_second);
        }
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0 && rss_pages > 0) {
          rss_bytes = static_cast<std::uint64_t>(rss_pages) *
                      static_cast<std::uint64_t>(page_size);
        }
        threads = thread_count;
      }
    }
  }

  metrics.setProcess(cpu_ms, rss_bytes, threads);
}
#else
void sampleProcessMetrics(Metrics& metrics) { (void)metrics; }
#endif

}  // namespace urlshort
