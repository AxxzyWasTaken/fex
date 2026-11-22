/*
  tests one thread rewriting code that another thread keeps running, as the SDM's cross-modifying code protocol allows:
  the writer stores the new code and then publishes its generation, the runner reads the generation, serializes and runs the
  code. The code must return at least the generation the runner saw. The JIT must not keep a translation that it decoded
  from the old code while the store was landing.
*/

#include <atomic>
#include <cpuid.h>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

typedef int UnalignedInt __attribute__((aligned(1)));

// Returns the first generation whose code the runner missed, or 0.
static int RewriteWhileRunning(char* code, char* rw) {
  memcpy(rw, "\xB8\x00\x00\x00\x00\xC3", 6); // mov eax, 0; ret
  std::atomic<int> generation {0};
  std::atomic<int> missed {0};
  std::atomic<bool> stop {false};

  std::thread runner([&] {
    while (!stop) {
      const int seen = generation.load(std::memory_order_acquire);
      unsigned a, b, c, d;
      __cpuid(0, a, b, c, d); // serialize
      if (((int (*)())code)() < seen && !missed) {
        missed = seen;
      }
    }
  });

  for (int i = 1; i <= 1000 && !missed; i++) {
    *reinterpret_cast<volatile UnalignedInt*>(rw + 1) = i; // the immediate, in one 32-bit store
    generation.store(i, std::memory_order_release);
    usleep(100);
  }
  stop = true;
  runner.join();
  return missed;
}

TEST_CASE("SMC: private_rwx") {
  auto code = (char*)mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(code != MAP_FAILED);
  CHECK(RewriteWhileRunning(code, code) == 0);
  munmap(code, 4096);
}
