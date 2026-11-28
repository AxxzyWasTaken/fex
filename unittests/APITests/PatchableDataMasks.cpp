// SPDX-License-Identifier: MIT
#include "Common/HostFeatures.h"
#include "Interface/Context/Context.h"
#include "Interface/Core/ArchHelpers/Arm64Emitter.h"
#include "Interface/Core/Frontend.h"
#include "Interface/Core/JIT/DebugData.h"
#include "Interface/Core/JIT/Relocations.h"
#include "Utils/crc32.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/Threads.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <pthread.h>
#include <span>
#include <unistd.h>

namespace {

uint64_t DecodeLogicalImm64(uint32_t Insn) {
  const uint32_t N = (Insn >> 22) & 1U;
  const uint32_t ImmR = (Insn >> 16) & 0x3FU;
  const uint32_t ImmS = (Insn >> 10) & 0x3FU;
  const uint32_t Combined = (N << 6) | (~ImmS & 0x3FU);

  int Len = 6;
  while (Len > 0 && ((Combined >> Len) & 1U) == 0) {
    --Len;
  }
  REQUIRE(Len >= 1);

  const uint32_t ESize = 1U << Len;
  const uint32_t Levels = ESize - 1U;
  const uint32_t S = ImmS & Levels;
  const uint32_t R = ImmR & Levels;
  const uint64_t WElement = (S + 1U == 64U) ? ~0ULL : ((1ULL << (S + 1U)) - 1ULL);
  uint64_t Rotated = R == 0 ? WElement : ((WElement >> R) | (WElement << (ESize - R)));
  if (ESize < 64U) {
    Rotated &= (1ULL << ESize) - 1ULL;
  }

  uint64_t Result = 0;
  for (uint32_t Shift = 0; Shift < 64U; Shift += ESize) {
    Result |= Rotated << Shift;
  }
  return Result;
}

uint64_t EvalPatchableSlot(std::span<const uint32_t> Insns, uint8_t ExpectedRegIdx) {
  bool Defined = false;
  uint64_t RegValue = 0xDEADBEEFCAFEBABEULL;

  for (uint32_t Insn : Insns) {
    if (Insn == 0xD503201FU) { // nop
      continue;
    }

    const uint8_t Rd = static_cast<uint8_t>(Insn & 0x1FU);
    REQUIRE(Rd == ExpectedRegIdx);

    if ((Insn & 0xFF800000U) == 0xD2800000U) { // movz xD, #imm16, lsl #(hw*16)
      const uint32_t HW = (Insn >> 21) & 3U;
      const uint64_t Imm16 = (Insn >> 5) & 0xFFFFU;
      RegValue = Imm16 << (HW * 16U);
      Defined = true;
    } else if ((Insn & 0xFF800000U) == 0x92800000U) { // movn xD, #imm16, lsl #(hw*16)
      const uint32_t HW = (Insn >> 21) & 3U;
      const uint64_t Imm16 = (Insn >> 5) & 0xFFFFU;
      RegValue = ~(Imm16 << (HW * 16U));
      Defined = true;
    } else if ((Insn & 0xFF800000U) == 0xF2800000U) { // movk xD, #imm16, lsl #(hw*16)
      REQUIRE(Defined);
      const uint32_t HW = (Insn >> 21) & 3U;
      const uint64_t Imm16 = (Insn >> 5) & 0xFFFFU;
      RegValue = (RegValue & ~(0xFFFFULL << (HW * 16U))) | (Imm16 << (HW * 16U));
    } else if ((Insn & 0xFF8003E0U) == 0xB20003E0U) { // orr xD, xzr, #imm
      RegValue = DecodeLogicalImm64(Insn);
      Defined = true;
    } else {
      FAIL("Unexpected ARM64 instruction emitted by LoadPatchableConstant");
    }
  }

  REQUIRE(Defined);
  return RegValue;
}

class TestSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override { }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

class TestSignalDelegator final : public FEXCore::SignalDelegator, public FEXCore::Allocator::FEXAllocOperators { };

class EmptyConfigLayer final : public FEXCore::Config::Layer {
public:
  explicit EmptyConfigLayer(FEXCore::Config::LayerType Type)
    : FEXCore::Config::Layer(Type) { }
  void Load() override { }
};

class TestThread final : public FEXCore::Threads::Thread, public FEXCore::Allocator::FEXAllocOperators {
public:
  TestThread(FEXCore::Threads::ThreadFunc Func, void* Arg) {
    Joinable = pthread_create(&Handle, nullptr, Func, Arg) == 0;
  }

  ~TestThread() override {
    if (Joinable) {
      pthread_join(Handle, nullptr);
    }
  }

  bool joinable() override {
    return Joinable;
  }

  bool join(void** Ret) override {
    if (!Joinable) {
      return false;
    }
    Joinable = false;
    return pthread_join(Handle, Ret) == 0;
  }

  bool detach() override {
    if (!Joinable) {
      return false;
    }
    Joinable = false;
    return pthread_detach(Handle) == 0;
  }

  bool IsSelf() override {
    return Joinable && pthread_equal(pthread_self(), Handle) != 0;
  }

private:
  pthread_t Handle {};
  bool Joinable {};
};

struct DiskCacheTestHarness {
  std::filesystem::path TempDir;
  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  fextl::unique_ptr<TestSignalDelegator> SignalDelegator;
  fextl::unique_ptr<TestSyscallHandler> SyscallHandler;
  FEXCore::Core::InternalThreadState* Thread {};
  FEXCore::Core::CPUState::gdt_segment GDT[32] {};

  DiskCacheTestHarness() {
    std::string Template = (std::filesystem::temp_directory_path() / "fex-patchable-test-XXXXXX").string();
    char* Made = mkdtemp(Template.data());
    REQUIRE(Made != nullptr);
    TempDir = Made;

    LogMan::Throw::InstallHandler([](const char* Message) { fprintf(stderr, "FEX ASSERT: %s\n", Message); });
    LogMan::Msg::InstallHandler([](LogMan::DebugLevels Level, const char* Message) {
      if (Level <= LogMan::ERROR) {
        fprintf(stderr, "FEX [%s]: %s\n", LogMan::DebugLevelStr(Level), Message);
      }
    });
    FEXCore::Threads::Thread::SetInternalPointers({
      .CreateThread = [](FEXCore::Threads::ThreadFunc Func, void* Arg, FEXCore::Threads::Flags,
                         const char*) -> fextl::unique_ptr<FEXCore::Threads::Thread> { return fextl::make_unique<TestThread>(Func, Arg); },
      .CleanupAfterFork = []() { },
    });
    FEXCore::Config::Initialize();
    FEXCore::Config::Load();
    FEXCore::Config::AddLayer(fextl::make_unique<EmptyConfigLayer>(FEXCore::Config::LayerType::LAYER_MAIN));
    FEXCore::Config::ReloadMetaLayer();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHEPATH, TempDir.string() + "/");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHEPRUNESTALEENTRIES, "0");

    auto HostFeatures = FEX::FetchHostFeatures(true);
    CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
    SignalDelegator = fextl::make_unique<TestSignalDelegator>();
    SyscallHandler = fextl::make_unique<TestSyscallHandler>();
    CTX->SetSignalDelegator(SignalDelegator.get());
    CTX->SetSyscallHandler(SyscallHandler.get());
    REQUIRE(CTX->InitCore());

    Thread = CTX->CreateThread();
    REQUIRE(Thread != nullptr);

    auto* Frame = Thread->CurrentFrame;
    Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = &GDT[0];
    Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = &GDT[0];
    Frame->State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* CS = FEXCore::Core::CPUState::GetSegmentFromIndex(Frame->State, Frame->State.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(CS, 0);
    FEXCore::Core::CPUState::SetGDTLimit(CS, 0xF'FFFFU);
    CS->L = 1;
    CS->D = 0;
    Frame->State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CS);
  }

  ~DiskCacheTestHarness() {
    if (Thread) {
      CTX->DestroyThread(Thread);
    }
    CTX.reset();
    FEXCore::Config::Shutdown();
    std::error_code EC;
    std::filesystem::remove_all(TempDir, EC);
  }

  FEXCore::Context::ContextImpl* Impl() const {
    return static_cast<FEXCore::Context::ContextImpl*>(CTX.get());
  }

  struct DecodeResult {
    fextl::vector<FEXCore::Frontend::Decoder::DataMask> Masks;
    bool FirstInstLiteralPatchable {};
    bool FirstInstGPRIndirectPatchable {};
    bool FirstInstSIBPatchable {};
    bool FirstInstRIPPatchable {};
  };

  DecodeResult DecodeSingleWithRet(std::span<const uint8_t> InstBytes, bool IsFileBacked) {
    fextl::vector<uint8_t> Code(InstBytes.begin(), InstBytes.end());
    Code.push_back(0xC3); // ret

    const uint64_t PC = reinterpret_cast<uint64_t>(Code.data());
    auto* Decoder = Thread->FrontendDecoder.get();
    Decoder->SetupDecodeInstructionsAtEntry(Thread, PC, 16, false, IsFileBacked);
    Decoder->DecodeLoop(Code.data());

    const auto* Info = Decoder->GetDecodedBlockInfo();
    REQUIRE(Info != nullptr);
    REQUIRE(Info->Blocks.size() == 1);
    const auto& Block = Info->Blocks[0];
    REQUIRE(Block.NumInstructions == 2);

    const auto& Inst = Block.DecodedInstructions[0];
    DecodeResult Res {};
    Res.Masks = Block.DataMasks;
    auto CheckOp = [&](const FEXCore::X86Tables::DecodedOperand& Op) {
      if (Op.IsLiteralPatchable()) {
        Res.FirstInstLiteralPatchable = true;
      }
      if (Op.IsGPRIndirect() && Op.Data.GPRIndirect.PatchableDisp) {
        Res.FirstInstGPRIndirectPatchable = true;
      }
      if (Op.IsSIB() && Op.Data.SIB.PatchableDisp) {
        Res.FirstInstSIBPatchable = true;
      }
      if (Op.IsRIPRelative() && Op.Data.RIPLiteral.PatchableDisp) {
        Res.FirstInstRIPPatchable = true;
      }
    };
    CheckOp(Inst.Dest);
    for (const auto& Src : Inst.Src) {
      CheckOp(Src);
    }

    Decoder->DelayedDisownBuffer();
    return Res;
  }
};

} // namespace

TEST_CASE("Arm64Emitter::LoadPatchableConstant exact sizes and values") {
  DiskCacheTestHarness Harness;
  std::array<uint32_t, 16> CodeBuf {};
  FEXCore::CPU::Arm64Emitter Emitter(Harness.Impl(), CodeBuf.data(), CodeBuf.size() * sizeof(uint32_t));
  const auto TestReg = ARMEmitter::Reg::r11;

  auto VerifyEmit = [&](uint64_t Value, uint8_t MaxInsns) {
    CodeBuf.fill(0xFFFFFFFFU);
    Emitter.SetCursorOffset(0);
    Emitter.LoadPatchableConstant(TestReg, Value, MaxInsns);
    REQUIRE(Emitter.GetCursorOffset() == static_cast<size_t>(MaxInsns) * 4U);
    const uint64_t Decoded = EvalPatchableSlot(std::span<const uint32_t>(CodeBuf.data(), MaxInsns), TestReg.Idx());
    CHECK(Decoded == Value);
  };

  SECTION("8-bit and 16-bit signed values in 1 instruction") {
    REQUIRE(FEXCore::CPU::Arm64Emitter::GetPatchableMoveInsns(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE, 1) == 1);
    REQUIRE(FEXCore::CPU::Arm64Emitter::GetPatchableMoveInsns(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE, 2) == 1);

    const std::array<int64_t, 11> Values = {
      0, 1, -1, 42, -42, 127, -128, 32767, -32768, 0xFFFF, -0x10000,
    };
    for (int64_t Val : Values) {
      const uint64_t UVal = static_cast<uint64_t>(Val);
      CHECK(FEXCore::CPU::Arm64Emitter::CanLoadPatchableConstant(UVal, 1));
      VerifyEmit(UVal, 1);
    }
    CHECK(FEXCore::CPU::Arm64Emitter::SignExtendPatchableData(0xFFULL, 1) == static_cast<uint64_t>(int64_t {-1}));
    CHECK(FEXCore::CPU::Arm64Emitter::SignExtendPatchableData(0x8000ULL, 2) == static_cast<uint64_t>(int64_t {-32768}));
  }

  SECTION("32-bit signed data and unsigned CRC values in 2 instructions") {
    REQUIRE(FEXCore::CPU::Arm64Emitter::GetPatchableMoveInsns(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE, 4) == 2);
    REQUIRE(FEXCore::CPU::Arm64Emitter::GetPatchableMoveInsns(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE, 4) == 2);

    const std::array<uint64_t, 16> Values = {
      0ULL,
      1ULL,
      0xFFFFULL,
      0x10000ULL,
      0x12345678ULL,
      0x7FFF'FFFFULL,
      0x8000'0000ULL,
      0xFFFF'0000ULL,
      0xFFFF'FFFFULL,
      static_cast<uint64_t>(int64_t {-1}),
      static_cast<uint64_t>(int64_t {-0xFFFF}),
      static_cast<uint64_t>(int64_t {-0x10000}),
      static_cast<uint64_t>(int64_t {-0x12345}),
      static_cast<uint64_t>(int64_t {std::numeric_limits<int32_t>::min()}),
      static_cast<uint64_t>(int64_t {std::numeric_limits<int32_t>::min() + 0xFFFF}),
      0xFFFF'FFFF'8765'4321ULL,
    };
    for (uint64_t UVal : Values) {
      CHECK(FEXCore::CPU::Arm64Emitter::CanLoadPatchableConstant(UVal, 2));
      VerifyEmit(UVal, 2);
    }
    CHECK(FEXCore::CPU::Arm64Emitter::SignExtendPatchableData(0x80000000ULL, 4) ==
          static_cast<uint64_t>(int64_t {std::numeric_limits<int32_t>::min()}));
  }

  SECTION("48-bit canonical RIP values in 3 instructions") {
    constexpr uint8_t ExpectedRIPInsns = 3;
    constexpr auto RIPMoveType = FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE;
    REQUIRE(FEXCore::CPU::Arm64Emitter::GetPatchableMoveInsns(RIPMoveType, 4) == ExpectedRIPInsns);

    const std::array<uint64_t, 8> Values = {
      0x0000'0000'0040'1000ULL,
      0x0000'7FFF'1234'5678ULL,
      0x0000'FFFF'FFFF'F000ULL,
      0xFFFF'FFFF'FF60'0000ULL,
      0xFFFF'8000'1234'5678ULL,
      0xFFFF'FFFF'FFFF'FFFFULL,
      0ULL,
      0x0000'1234'0000'5678ULL,
    };
    for (uint64_t UVal : Values) {
      CHECK(FEXCore::CPU::Arm64Emitter::CanLoadPatchableConstant(UVal, 3));
      VerifyEmit(UVal, 3);
    }
    CHECK_FALSE(FEXCore::CPU::Arm64Emitter::CanLoadPatchableConstant(0x0123'4567'89AB'CDEFULL, 3));
  }

  SECTION("64-bit values in 4 instructions") {
    REQUIRE(FEXCore::CPU::Arm64Emitter::GetPatchableMoveInsns(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE, 8) == 4);

    const std::array<uint64_t, 10> Values = {
      0ULL,
      ~0ULL,
      static_cast<uint64_t>(std::numeric_limits<int64_t>::min()),
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
      0x0123'4567'89AB'CDEFULL,
      0xFEDC'BA98'7654'3210ULL,
      0xFFFF'FFFF'0123'FFFFULL,
      0x5555'5555'5555'5555ULL,
      0x0001'0002'0003'0004ULL,
      0xFFFE'FFFD'FFFC'FFFBULL,
    };
    for (uint64_t UVal : Values) {
      CHECK(FEXCore::CPU::Arm64Emitter::CanLoadPatchableConstant(UVal, 4));
      VerifyEmit(UVal, 4);
    }
  }

  SECTION("ApplyPackedCodeRelocations patches compact slots with live guest bytes") {
    alignas(8) std::array<uint8_t, 16> GuestBytes {};
    const int32_t LiveDisp = -0x23456;
    const uint64_t LiveImm64 = 0x1122'3344'5566'7788ULL;
    const int8_t LiveImm8 = -17;
    std::memcpy(&GuestBytes[0], &LiveImm8, 1);
    std::memcpy(&GuestBytes[4], &LiveDisp, 4);
    std::memcpy(&GuestBytes[8], &LiveImm64, 8);

    // Layout: [0]=1-insn data(1B), [4]=2-insn data(4B), [12]=4-insn data(8B),
    //         [28]=2-insn crc(4B), [36]=RIPInsns rip(4B)
    constexpr auto RIPMoveType = FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE;
    const uint8_t RIPInsns = FEXCore::CPU::Arm64Emitter::GetPatchableMoveInsns(RIPMoveType, 4);
    fextl::vector<uint32_t> HostCode(9U + RIPInsns, 0xD503201FU);

    std::array<FEXCore::DiskCache::BlobSmallRelocation, 5> Relocs {};
    Relocs[0].Offset = 0;
    Relocs[0].Type = static_cast<uint8_t>(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE);
    Relocs[0].PatchableData = {.RegisterIndex = 5, .ValueSize = 1, .SiteOffset = 0};

    Relocs[1].Offset = 4;
    Relocs[1].Type = static_cast<uint8_t>(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE);
    Relocs[1].PatchableData = {.RegisterIndex = 6, .ValueSize = 4, .SiteOffset = 4};

    Relocs[2].Offset = 12;
    Relocs[2].Type = static_cast<uint8_t>(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE);
    Relocs[2].PatchableData = {.RegisterIndex = 7, .ValueSize = 8, .SiteOffset = 8};

    Relocs[3].Offset = 28;
    Relocs[3].Type = static_cast<uint8_t>(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE);
    Relocs[3].PatchableData = {.RegisterIndex = 8, .ValueSize = 4, .SiteOffset = 4};

    Relocs[4].Offset = 36;
    Relocs[4].Type = static_cast<uint8_t>(FEXCore::CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE);
    Relocs[4].PatchableData = {.RegisterIndex = 9, .ValueSize = 4, .SiteOffset = 4};

    const uint64_t GuestBase = reinterpret_cast<uint64_t>(GuestBytes.data());
    REQUIRE(Harness.Impl()->CodeCache.ApplyPackedCodeRelocations(GuestBase, std::as_writable_bytes(std::span(HostCode)), Relocs, {}));

    CHECK(EvalPatchableSlot(std::span<const uint32_t>(&HostCode[0], 1), 5) == static_cast<uint64_t>(int64_t {LiveImm8}));
    CHECK(EvalPatchableSlot(std::span<const uint32_t>(&HostCode[1], 2), 6) == static_cast<uint64_t>(int64_t {LiveDisp}));
    CHECK(EvalPatchableSlot(std::span<const uint32_t>(&HostCode[3], 4), 7) == LiveImm64);
    CHECK(EvalPatchableSlot(std::span<const uint32_t>(&HostCode[7], 2), 8) == FEXCore::Utils::crc32(&GuestBytes[4], 4));
    const uint64_t ExpectedRIPTarget = GuestBase + 4ULL + 4ULL + static_cast<uint64_t>(int64_t {LiveDisp});
    CHECK(EvalPatchableSlot(std::span<const uint32_t>(&HostCode[9], RIPInsns), 9) == ExpectedRIPTarget);
  }
}

TEST_CASE("Frontend::Decoder::DetectDataMasks filtering heuristics") {
  DiskCacheTestHarness Harness;
  REQUIRE(Harness.Impl()->DiskCache.IsReadingDiskCache());
  REQUIRE(Harness.Impl()->DiskCache.IsWritingDiskCache());

  SECTION("cmp *, imm8 masked only for anonymous mappings") {
    // cmp al, 0x42 (0x80 /7)
    const std::array<uint8_t, 3> CmpImm8_80 = {0x80, 0xF8, 0x42};
    // cmp rax, 0x42 (0x83 /7)
    const std::array<uint8_t, 4> CmpImm8_83 = {0x48, 0x83, 0xF8, 0x42};

    auto Anon80 = Harness.DecodeSingleWithRet(CmpImm8_80, false);
    CHECK(Anon80.Masks.size() == 1);
    CHECK(Anon80.FirstInstLiteralPatchable);

    auto File80 = Harness.DecodeSingleWithRet(CmpImm8_80, true);
    CHECK(File80.Masks.empty());
    CHECK_FALSE(File80.FirstInstLiteralPatchable);

    auto Anon83 = Harness.DecodeSingleWithRet(CmpImm8_83, false);
    CHECK(Anon83.Masks.size() == 1);
    CHECK(Anon83.FirstInstLiteralPatchable);

    auto File83 = Harness.DecodeSingleWithRet(CmpImm8_83, true);
    CHECK(File83.Masks.empty());
    CHECK_FALSE(File83.FirstInstLiteralPatchable);
  }

  SECTION("Small signed immediates in (-0x10000, +0x10000) exempted for all FLAGS_LITERAL_PATCHABLE ops") {
    // sub rsp, 0x100
    const std::array<uint8_t, 7> SubRspSmall = {0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};
    // and eax, 0xfff (0x25)
    const std::array<uint8_t, 5> AndEaxSmall = {0x25, 0xFF, 0x0F, 0x00, 0x00};
    // cmp eax, -1 (0x81 /7, 32-bit dest)
    const std::array<uint8_t, 6> CmpEaxNeg1 = {0x81, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};
    // cmp rax, -0xffff (0x81 /7, 64-bit dest)
    const std::array<uint8_t, 7> CmpRaxNegFFFF = {0x48, 0x81, 0xF8, 0x01, 0x00, 0xFF, 0xFF};
    // mov eax, -1 (0xB8)
    const std::array<uint8_t, 5> MovEaxNeg1 = {0xB8, 0xFF, 0xFF, 0xFF, 0xFF};
    // mov rax, -0x100 (imm64)
    const std::array<uint8_t, 10> MovRaxNeg100 = {0x48, 0xB8, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    CHECK(Harness.DecodeSingleWithRet(SubRspSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(AndEaxSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(CmpEaxNeg1, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(CmpRaxNegFFFF, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(MovEaxNeg1, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(MovRaxNeg100, false).Masks.empty());

    // Boundary & large values MUST still be masked:
    // sub rsp, 0x10000
    const std::array<uint8_t, 7> SubRspBound = {0x48, 0x81, 0xEC, 0x00, 0x00, 0x01, 0x00};
    // cmp eax, -0x10000
    const std::array<uint8_t, 6> CmpEaxNegBound = {0x81, 0xF8, 0x00, 0x00, 0xFF, 0xFF};
    // cmp eax, INT32_MIN (-0x80000000)
    const std::array<uint8_t, 6> CmpEaxInt32Min = {0x81, 0xF8, 0x00, 0x00, 0x00, 0x80};
    // mov rax, 0x00007fff12345678
    const std::array<uint8_t, 10> MovRaxPtr = {0x48, 0xB8, 0x78, 0x56, 0x34, 0x12, 0xFF, 0x7F, 0x00, 0x00};

    CHECK(Harness.DecodeSingleWithRet(SubRspBound, false).FirstInstLiteralPatchable);
    CHECK(Harness.DecodeSingleWithRet(CmpEaxNegBound, false).FirstInstLiteralPatchable);
    CHECK(Harness.DecodeSingleWithRet(CmpEaxInt32Min, false).FirstInstLiteralPatchable);
    CHECK(Harness.DecodeSingleWithRet(MovRaxPtr, false).FirstInstLiteralPatchable);
  }

  SECTION("Small signed disp32 exempted for GPRIndirect and SIB, never RIP-relative") {
    // GPRIndirect: mov eax, [rbp + disp32]
    const std::array<uint8_t, 6> GPRPosSmall = {0x8B, 0x85, 0x00, 0x01, 0x00, 0x00};    // +0x100
    const std::array<uint8_t, 6> GPRNegSmall = {0x8B, 0x85, 0x00, 0xFF, 0xFF, 0xFF};    // -0x100
    const std::array<uint8_t, 6> GPRPosMaxSmall = {0x8B, 0x85, 0xFF, 0xFF, 0x00, 0x00}; // +0xffff
    const std::array<uint8_t, 6> GPRNegMinSmall = {0x8B, 0x85, 0x01, 0x00, 0xFF, 0xFF}; // -0xffff
    const std::array<uint8_t, 6> GPRPosLarge = {0x8B, 0x85, 0x00, 0x00, 0x01, 0x00};    // +0x10000
    const std::array<uint8_t, 6> GPRNegLarge = {0x8B, 0x85, 0x00, 0x00, 0xFF, 0xFF};    // -0x10000
    const std::array<uint8_t, 6> GPRInt32Min = {0x8B, 0x85, 0x00, 0x00, 0x00, 0x80};    // INT32_MIN

    CHECK(Harness.DecodeSingleWithRet(GPRPosSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(GPRNegSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(GPRPosMaxSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(GPRNegMinSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(GPRPosLarge, false).FirstInstGPRIndirectPatchable);
    CHECK(Harness.DecodeSingleWithRet(GPRNegLarge, false).FirstInstGPRIndirectPatchable);
    CHECK(Harness.DecodeSingleWithRet(GPRInt32Min, false).FirstInstGPRIndirectPatchable);

    // SIB mod=10: mov eax, [rsp + rcx*8 + disp32]
    const std::array<uint8_t, 7> SIBPosSmall = {0x8B, 0x84, 0xCC, 0x00, 0x01, 0x00, 0x00}; // +0x100
    const std::array<uint8_t, 7> SIBNegSmall = {0x8B, 0x84, 0xCC, 0x00, 0xFF, 0xFF, 0xFF}; // -0x100
    const std::array<uint8_t, 7> SIBPosLarge = {0x8B, 0x84, 0xCC, 0x00, 0x00, 0x01, 0x00}; // +0x10000
    const std::array<uint8_t, 7> SIBNegLarge = {0x8B, 0x84, 0xCC, 0x00, 0x00, 0xFF, 0xFF}; // -0x10000
    const std::array<uint8_t, 7> SIBInt32Min = {0x8B, 0x84, 0xCC, 0x00, 0x00, 0x00, 0x80}; // INT32_MIN
    // SIB mod=0, base=5: mov rax, gs:[0x30] and mov eax, [rcx*8 + 0x20000]
    const std::array<uint8_t, 9> SIBGS30 = {0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00};
    const std::array<uint8_t, 7> SIBNoBaseLarge = {0x8B, 0x04, 0xCD, 0x00, 0x00, 0x02, 0x00};

    CHECK(Harness.DecodeSingleWithRet(SIBPosSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(SIBNegSmall, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(SIBGS30, false).Masks.empty());
    CHECK(Harness.DecodeSingleWithRet(SIBPosLarge, false).FirstInstSIBPatchable);
    CHECK(Harness.DecodeSingleWithRet(SIBNegLarge, false).FirstInstSIBPatchable);
    CHECK(Harness.DecodeSingleWithRet(SIBInt32Min, false).FirstInstSIBPatchable);
    CHECK(Harness.DecodeSingleWithRet(SIBNoBaseLarge, false).FirstInstSIBPatchable);

    // RIP-relative MUST ALWAYS be masked even for tiny displacements
    const std::array<uint8_t, 6> RIPPosSmall = {0x8B, 0x05, 0x10, 0x00, 0x00, 0x00}; // [rip + 0x10]
    const std::array<uint8_t, 6> RIPNegSmall = {0x8B, 0x05, 0xF0, 0xFF, 0xFF, 0xFF}; // [rip - 0x10]

    auto RIPPosRes = Harness.DecodeSingleWithRet(RIPPosSmall, true);
    CHECK(RIPPosRes.Masks.size() == 1);
    CHECK(RIPPosRes.FirstInstRIPPatchable);

    auto RIPNegRes = Harness.DecodeSingleWithRet(RIPNegSmall, true);
    CHECK(RIPNegRes.Masks.size() == 1);
    CHECK(RIPNegRes.FirstInstRIPPatchable);
  }
}

TEST_CASE("DiskCache store, reload, and bit-identical patching with compact slots") {
  DiskCacheTestHarness Harness;
  REQUIRE(Harness.Impl()->DiskCache.IsReadingDiskCache());
  REQUIRE(Harness.Impl()->DiskCache.IsWritingDiskCache());

  // Guest block exercising all compact patchable relocation sizes (all values live at ret):
  //   [0..6]   sub rsp, 0x20000            (4B pos imm32 -> 2-insn DATA_MOVE)
  //   [7..12]  mov eax, [rbp - 0x20000]    (4B neg disp32 -> 2-insn DATA_MOVE, sign-extended)
  //   [13..22] mov rdx, 0x00007fff12345678 (8B imm64 -> 4-insn DATA_MOVE)
  //   [23..28] mov ecx, [rip + 0x20]       (4B RIP disp32 -> 3-insn RIP_MOVE)
  //   [29..31] cmp al, 0x42                (1B imm8, anon -> 1-insn DATA_MOVE; flags live at ret)
  //   [32]     ret
  std::array<uint8_t, 34> Guest1 = {
    0x48, 0x81, 0xEC, 0x00, 0x00, 0x02, 0x00,                   // sub rsp, 0x20000
    0x8B, 0x85, 0x00, 0x00, 0xFE, 0xFF,                         // mov eax, [rbp - 0x20000]
    0x48, 0xBA, 0x78, 0x56, 0x34, 0x12, 0xFF, 0x7F, 0x00, 0x00, // mov rdx, 0x00007fff12345678
    0x8B, 0x0D, 0x20, 0x00, 0x00, 0x00,                         // mov ecx, [rip + 0x20]
    0x80, 0xF8, 0x42,                                           // cmp al, 0x42
    0xC3, 0x90,
  };

  // Same instruction skeleton at a different address with mutated masked constants (positive & negative).
  std::array<uint8_t, 34> Guest2 = {
    0x48, 0x81, 0xEC, 0x67, 0x45, 0x03, 0x00,                   // sub rsp, 0x34567
    0x8B, 0x85, 0x88, 0xA9, 0xFB, 0xFF,                         // mov eax, [rbp - 0x45678]
    0x48, 0xBA, 0x21, 0x43, 0x65, 0x87, 0xFE, 0x7F, 0x00, 0x00, // mov rdx, 0x00007ffe87654321
    0x8B, 0x0D, 0xE0, 0xFF, 0xFF, 0xFF,                         // mov ecx, [rip - 0x20]
    0x80, 0xF8, 0x81,                                           // cmp al, -127 (0x81)
    0xC3, 0x90,
  };

  // Compile Guest1 to populate the DiskCache.
  const uint64_t PC1 = reinterpret_cast<uint64_t>(Guest1.data());
  REQUIRE(Harness.Impl()->CompileBlock(Harness.Thread->CurrentFrame, PC1, 500) != 0);

  // Look up Guest2 (different address + mutated masked fields) and verify cache hit.
  const uint64_t PC2 = reinterpret_cast<uint64_t>(Guest2.data());
  Harness.Thread->FrontendDecoder->SetupDecodeInstructionsAtEntry(Harness.Thread, PC2, 500, true, false);
  std::optional<uint64_t> Key2;
  auto Hit = Harness.Impl()->DiskCache.Lookup(Harness.Thread, std::nullopt, PC2, Key2);
  Harness.Thread->FrontendDecoder->DelayedDisownBuffer();
  REQUIRE(Hit.has_value());
  REQUIRE(Hit->HostCode.size() > 0);

  // Live-compile Guest2 and verify the re-patched cached host code matches live JIT code bit-for-bit.
  Harness.Thread->FrontendDecoder->SetupDecodeInstructionsAtEntry(Harness.Thread, PC2, 500, true, false);
  auto Live = Harness.Impl()->CompileCode(Harness.Thread, PC2, 500);
  REQUIRE(Live.CompiledCode.BlockBegin != nullptr);
  REQUIRE(Hit->HostCode.size() == Live.CompiledCode.Size);
  CHECK(std::memcmp(Hit->HostCode.data(), Live.CompiledCode.BlockBegin, Live.CompiledCode.Size) == 0);
}
