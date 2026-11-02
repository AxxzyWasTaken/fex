// A call followed by padding usually doesn't return: the next function starts after the padding, so FEX shouldn't translate it with the
// caller. If the call does return, the padding and what follows it still have to run. They just need their own translation then.
//
// The difference is invisible to the guest other than in speed, so the test reads FEX's own compile count.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" uint64_t call_padding_callee(uint64_t);
__asm__(R"(
.text
call_padding_callee:
  ret
)");

// With 0 this takes the path around the call and returns 0. With 1 it makes the call and returns 1 from what follows it.
#define CALL_THEN(Name, Bytes)                      \
  extern "C" uint64_t Name(uint64_t);               \
  __asm__(".text\n" #Name ":\n"                     \
          "  test rdi, rdi\n"                       \
          "  jz 1f\n"                               \
          "  call call_padding_callee\n" Bytes "\n" \
          "  mov eax, 1\n"                          \
          "  ret\n"                                 \
          "1:\n"                                    \
          "  xor eax, eax\n"                        \
          "  ret\n")

CALL_THEN(call_then_code, "  mov ecx, 1\n");
CALL_THEN(call_then_nop, ".byte 0x90\n");
CALL_THEN(call_then_long_nop, ".byte 0x0F, 0x1F, 0x00\n");
CALL_THEN(call_then_prefixed_nop, ".byte 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00\n");
CALL_THEN(call_then_segment_nop, ".byte 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00\n");
CALL_THEN(call_then_int3, ".byte 0xCC\n");

// From FEXCore/Utils/SHMStats.h, which only ever appends to these. A guest can't include it, it would need FEX's arch defines.
struct StatsHeader {
  uint8_t Version;
  uint8_t AppType;
  uint16_t ThreadStatsSize;
  char FEXVersion[48];
  uint32_t Head;
};

constexpr size_t JITCountOffset = 8 + 8 * 8;

static uint64_t CompileCount(const uint8_t* Stats) {
  auto Header = reinterpret_cast<const StatsHeader*>(Stats);
  uint64_t Count {};
  for (uint32_t Offset = std::atomic_ref(const_cast<uint32_t&>(Header->Head)).load(); Offset;) {
    auto Thread = Stats + Offset;
    Count += std::atomic_ref(*const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(Thread + JITCountOffset))).load();
    Offset = std::atomic_ref(*const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(Thread))).load();
  }
  return Count;
}

struct Result {
  uint64_t Returned;
  uint64_t Compiles;
};

static Result Measure(const uint8_t* Stats, uint64_t (*Function)(uint64_t)) {
  const uint64_t Before = Stats ? CompileCount(Stats) : 0;
  const uint64_t Returned = Function(1);
  return {Returned, Stats ? CompileCount(Stats) - Before : 0};
}

static const uint8_t* MapStats() {
  // CMake sets this for this test. Without it this isn't running under FEX's test harness.
  if (!getenv("FEX_PROFILESTATS")) {
    return nullptr;
  }

  char Path[64];
  snprintf(Path, sizeof(Path), "/dev/shm/fex-%d-stats", getpid());
  int FD = open(Path, O_RDONLY);
  REQUIRE(FD != -1);

  struct stat Info {};
  REQUIRE(fstat(FD, &Info) == 0);
  auto Stats = static_cast<const uint8_t*>(mmap(nullptr, Info.st_size, PROT_READ, MAP_SHARED, FD, 0));
  REQUIRE(Stats != MAP_FAILED);
  return Stats;
}

static volatile sig_atomic_t Traps;

TEST_CASE("What follows padding is translated when a call returns to it") {
  const uint8_t* Stats = MapStats();
  signal(SIGTRAP, [](int) { Traps = Traps + 1; });

  // Translate everything Measure runs other than what follows the calls.
  (void)Measure(Stats, call_padding_callee);
  (void)Measure(Stats, call_padding_callee);
  for (auto Function : {call_then_code, call_then_nop, call_then_long_nop, call_then_prefixed_nop, call_then_segment_nop, call_then_int3}) {
    REQUIRE(Function(0) == 0);
  }

  const auto Code = Measure(Stats, call_then_code);
  CHECK(Code.Returned == 1);
  if (Stats) {
    CHECK(Code.Compiles == 0);
  }

  for (auto Function : {call_then_nop, call_then_long_nop, call_then_prefixed_nop, call_then_segment_nop}) {
    const auto Padded = Measure(Stats, Function);
    CHECK(Padded.Returned == 1);
    if (Stats) {
      CHECK(Padded.Compiles >= 1);
    }
  }

  // The signal handler compiles too, so only check this one runs.
  CHECK(call_then_int3(1) == 1);
  CHECK(Traps == 1);
}
