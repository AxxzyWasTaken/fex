// SPDX-License-Identifier: MIT
#pragma once

#include "Interface/IR/IR.h"
#include <array>
#include <cstdint>

namespace FEXCore::IR {
class IREmitter;

struct AddressMode {
  Ref Segment {nullptr};
  Ref Base {nullptr};
  Ref Index {nullptr};
  int64_t Offset = 0;

  MemOffsetType IndexType = MemOffsetType::SXTX;
  uint8_t IndexScale = 1;

  // Size in bytes for the address calculation. 8 for an arm64 hardware mode.
  IR::OpSize AddrSize {};
  bool NonTSO {};

  // Metadata for intra-block SIB/segment base CSE (valid only when Cacheable is true).
  static constexpr uint8_t InvalidGPR = 0xFF;
  uint8_t BaseGPR = InvalidGPR;
  uint8_t IndexGPR = InvalidGPR;
  uint8_t SegmentPrefix = 0;
  bool Cacheable = false;

  [[nodiscard]]
  bool HasBase() const {
    return Base != nullptr || BaseGPR != InvalidGPR;
  }

  [[nodiscard]]
  bool HasIndex() const {
    return Index != nullptr || IndexGPR != InvalidGPR;
  }
};

struct AddrCacheKey {
  uint32_t BaseVer {};
  uint32_t IndexVer {};
  uint32_t SegVer {};
  uint8_t BaseGPR {AddressMode::InvalidGPR};
  uint8_t IndexGPR {AddressMode::InvalidGPR};
  uint8_t IndexScale {1};
  uint8_t SegmentPrefix {0};
  IR::OpSize AddrSize {};
  IR::OpSize GPRSize {};

  bool operator==(const AddrCacheKey&) const = default;
};

struct AddrBaseCache {
  static constexpr size_t MaxEntries = 4;
  static constexpr uint32_t MaxWindow = 10;

  struct Entry {
    AddrCacheKey Key {};
    Ref Value {nullptr};
    uint32_t LastUseInstIndex {};
  };

  std::array<Entry, MaxEntries> Entries {};
  size_t Count {};
  std::array<uint32_t, 16> GPRVersion {};
  uint32_t SegVersion {};

  void Reset() {
    Count = 0;
  }

  void InvalidateGPR(uint8_t GPR) {
    if (GPR < GPRVersion.size()) {
      ++GPRVersion[GPR];
    }
  }

  void InvalidateSegments() {
    ++SegVersion;
  }

  [[nodiscard]]
  bool IsEntryLive(const Entry& E, uint32_t CurInstIndex) const {
    if ((CurInstIndex - E.LastUseInstIndex) > MaxWindow) {
      return false;
    }
    if (E.Key.BaseGPR < GPRVersion.size() && E.Key.BaseVer != GPRVersion[E.Key.BaseGPR]) {
      return false;
    }
    if (E.Key.IndexGPR < GPRVersion.size() && E.Key.IndexVer != GPRVersion[E.Key.IndexGPR]) {
      return false;
    }
    if (E.Key.SegmentPrefix != 0 && E.Key.SegVer != SegVersion) {
      return false;
    }
    return true;
  }

  [[nodiscard]]
  Ref Lookup(const AddrCacheKey& Key, uint32_t CurInstIndex) {
    for (size_t i = 0; i < Count; ++i) {
      if (Entries[i].Key == Key) {
        if ((CurInstIndex - Entries[i].LastUseInstIndex) > MaxWindow) {
          return nullptr;
        }
        Entry Hit = Entries[i];
        Hit.LastUseInstIndex = CurInstIndex;
        for (size_t j = i; j > 0; --j) {
          Entries[j] = Entries[j - 1];
        }
        Entries[0] = Hit;
        return Hit.Value;
      }
    }
    return nullptr;
  }

  void Insert(const AddrCacheKey& Key, Ref Value, uint32_t CurInstIndex) {
    size_t EvictIdx = Count < MaxEntries ? Count : (MaxEntries - 1);
    for (size_t i = 0; i < Count; ++i) {
      if (Entries[i].Key == Key || !IsEntryLive(Entries[i], CurInstIndex)) {
        EvictIdx = i;
        break;
      }
    }
    if (EvictIdx == Count && Count < MaxEntries) {
      ++Count;
    }
    for (size_t j = EvictIdx; j > 0; --j) {
      Entries[j] = Entries[j - 1];
    }
    Entries[0] = Entry {
      .Key = Key,
      .Value = Value,
      .LastUseInstIndex = CurInstIndex,
    };
  }
};

using MaterializeGPRFn = Ref (*)(void* Ctx, uint8_t GPR, IR::OpSize GPRSize);

struct AddrCacheContext {
  AddrBaseCache* Cache {nullptr};
  uint32_t InstIndex {};
  MaterializeGPRFn LoadGPR {nullptr};
  void* Ctx {nullptr};
};

Ref LoadEffectiveAddress(IREmitter* IREmit, const AddressMode& A, IR::OpSize GPRSize, bool AddSegmentBase, bool AllowUpperGarbage = false,
                         const AddrCacheContext* CacheCtx = nullptr);
AddressMode SelectAddressMode(IREmitter* IREmit, const AddressMode& A, IR::OpSize GPRSize, bool HostSupportsTSOImm9, bool AtomicTSO,
                              bool Vector, IR::OpSize AccessSize, const AddrCacheContext* CacheCtx = nullptr);

} // namespace FEXCore::IR