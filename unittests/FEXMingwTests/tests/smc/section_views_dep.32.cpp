#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <windows.h>

// With DEP off, readable memory is executable, including views of non-image sections mapped afterwards.
TEST_CASE("A readable view runs with DEP disabled") {
  // Wine's SetProcessDEPPolicy(0) passes no flags, which its NtSetInformationProcess refuses, so set them directly.
  using SetInfoFn = LONG(WINAPI*)(HANDLE, ULONG, void*, ULONG);
  auto NtSetInformationProcess = reinterpret_cast<SetInfoFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess"));
  REQUIRE(NtSetInformationProcess);
  ULONG Flags = 2; // MEM_EXECUTE_OPTION_ENABLE
  if (NtSetInformationProcess(GetCurrentProcess(), 34 /* ProcessExecuteFlags */, &Flags, sizeof(Flags))) {
    SKIP("DEP can't be disabled in this process");
  }
  HANDLE Section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, 0x10000, nullptr);
  REQUIRE(Section);
  auto* RW = MapViewOfFile(Section, FILE_MAP_WRITE, 0, 0, 0);
  auto* R = MapViewOfFile(Section, FILE_MAP_READ, 0, 0, 0);
  REQUIRE(RW);
  REQUIRE(R);

  const uint8_t Code[] = {0xb8, 0x7b, 0, 0, 0, 0xc3}; // mov eax, 123; ret
  memcpy(RW, Code, sizeof(Code));
  CHECK(reinterpret_cast<uint32_t (*)()>(R)() == 123);

  UnmapViewOfFile(R);
  UnmapViewOfFile(RW);
  CloseHandle(Section);
}
