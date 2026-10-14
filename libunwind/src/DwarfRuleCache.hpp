//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//
//  ClickHouse: cache of decoded DWARF unwind rules, keyed by instruction
//  pointer.
//
//===----------------------------------------------------------------------===//

#ifndef __DWARF_RULE_CACHE_HPP__
#define __DWARF_RULE_CACHE_HPP__

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "DwarfParser.hpp"
#include "libunwind.h"

// See `_LIBUNWIND_USE_DWARF_RULE_CACHE` in config.h.
#if defined(_LIBUNWIND_USE_DWARF_RULE_CACHE)

namespace libunwind {

/// Stepping over a frame finds and interprets its FDE twice: once to fill the
/// proc info and once to compute the caller's registers. Both results depend
/// only on the instruction pointer, so they are kept here after the first
/// time.
///
/// Rules are only cached for FDEs found in a loaded module. ClickHouse never
/// unloads modules (its `dl_iterate_phdr` cache relies on the same), so an
/// entry cannot go stale.
///
/// Lookups and inserts take no lock and do not allocate, so they are safe in
/// signal handlers. Every slot is a seqlock: a lookup that races with a write
/// misses, and an insert into a slot that is being written is dropped.
template <typename A> class DwarfRuleCache {
public:
  typedef typename A::pint_t pint_t;
  typedef typename CFI_Parser<A>::PrologInfo PrologInfo;
  typedef typename CFI_Parser<A>::CIE_Info CIE_Info;

#if defined(_LIBUNWIND_TARGET_AARCH64)
  // x19-x30, d8-d15 and the return address signing state.
  static constexpr uint8_t kMaxSaved = 24;
#else
  static constexpr uint8_t kMaxSaved = 12;
#endif

  struct Rule {
    // Proc info of the frame.
    pint_t startIP;
    pint_t endIP;
    pint_t lsda;
    pint_t handler;
    pint_t unwindInfo;
    pint_t extra;
    uint32_t unwindInfoSize;
    // `unw_proc_info_t::gp`, from the FDE interpreted where the proc info was
    // looked up; the step interprets it at a different PC.
    uint32_t gp;
    // What the step needs from the interpreted FDE and its CIE.
    uint32_t cfaRegister;
    int32_t cfaRegisterOffset;
#if defined(_LIBUNWIND_TARGET_AARCH64)
    pint_t ptrAuthDiversifier;
    bool addressesSignedWithBKey;
    bool mteTaggedFrame;
#endif
    uint8_t returnAddressRegister;
    bool isSignalFrame;
    // The step needs more than `DwarfInstructions::applyCachedRule` does
    // (compute the CFA, restore the saved registers, set the new IP): AArch64
    // return address authentication or MTE tag clearing. Such a rule is
    // expanded into a `PrologInfo` and applied by `applyRule` instead.
    bool needsApplyRule;
    uint8_t numSaved;
    struct Saved {
      uint8_t reg;
      uint8_t location;
      int32_t value;
    } saved[kMaxSaved];
  };

  /// A return address is looked up one byte back, so the same address maps to
  /// a different rule when it is not a return address. The complement of a
  /// user space address is never a user space address.
  static pint_t makeKey(pint_t ip, bool isReturnAddress) {
    return isReturnAddress ? ip : ~ip;
  }

  /// Returns false for rules that need a DWARF expression or do not fit.
  static bool makeRule(const PrologInfo &prolog, const CIE_Info &cieInfo,
                       const unw_proc_info_t &info, Rule &rule) {
    if (prolog.cfaRegister == (uint32_t)(-1))
      return false;
    memset(&rule, 0, sizeof(rule));
    for (uint32_t reg = 0; reg <= CFI_Parser<A>::kMaxRegisterNumber; ++reg) {
      const auto &saved = prolog.savedRegisters[reg];
      if (saved.location == CFI_Parser<A>::kRegisterUnused && saved.value == 0)
        continue;
      if (saved.location == CFI_Parser<A>::kRegisterAtExpression ||
          saved.location == CFI_Parser<A>::kRegisterIsExpression ||
          saved.location == CFI_Parser<A>::kRegisterInCFADecrypt ||
          saved.value != (int32_t)saved.value || rule.numSaved == kMaxSaved)
        return false;
      rule.saved[rule.numSaved++] = {(uint8_t)reg, (uint8_t)saved.location,
                                     (int32_t)saved.value};
    }
    rule.startIP = (pint_t)info.start_ip;
    rule.endIP = (pint_t)info.end_ip;
    rule.lsda = (pint_t)info.lsda;
    rule.handler = (pint_t)info.handler;
    rule.unwindInfo = (pint_t)info.unwind_info;
    rule.extra = (pint_t)info.extra;
    rule.unwindInfoSize = info.unwind_info_size;
    rule.gp = (uint32_t)info.gp;
    rule.cfaRegister = prolog.cfaRegister;
    rule.cfaRegisterOffset = prolog.cfaRegisterOffset;
#if defined(_LIBUNWIND_TARGET_AARCH64)
    rule.ptrAuthDiversifier = prolog.ptrAuthDiversifier;
    rule.addressesSignedWithBKey = cieInfo.addressesSignedWithBKey;
    rule.mteTaggedFrame = cieInfo.mteTaggedFrame;
#endif
    rule.returnAddressRegister = cieInfo.returnAddressRegister;
    rule.isSignalFrame = cieInfo.isSignalFrame;
#if defined(__x86_64__)
    rule.needsApplyRule = false;
#elif defined(__aarch64__)
    const auto &raSignState = prolog.savedRegisters[UNW_AARCH64_RA_SIGN_STATE];
    rule.needsApplyRule =
        cieInfo.mteTaggedFrame ||
        raSignState.location != CFI_Parser<A>::kRegisterUnused ||
        raSignState.value != 0;
#else
    rule.needsApplyRule = true;
#endif
    return true;
  }

  /// `prolog` must be freshly constructed: registers not in the rule stay
  /// unused.
  static void expandRule(const Rule &rule, PrologInfo &prolog,
                         CIE_Info &cieInfo) {
    prolog.cfaRegister = rule.cfaRegister;
    prolog.cfaRegisterOffset = rule.cfaRegisterOffset;
#if defined(_LIBUNWIND_TARGET_AARCH64)
    prolog.ptrAuthDiversifier = rule.ptrAuthDiversifier;
#endif
    for (uint8_t i = 0; i < rule.numSaved; ++i) {
      auto &saved = prolog.savedRegisters[rule.saved[i].reg];
      saved.location =
          (typename CFI_Parser<A>::RegisterSavedWhere)rule.saved[i].location;
      saved.value = rule.saved[i].value;
    }
    memset(&cieInfo, 0, sizeof(cieInfo));
#if defined(_LIBUNWIND_TARGET_AARCH64)
    cieInfo.addressesSignedWithBKey = rule.addressesSignedWithBKey;
    cieInfo.mteTaggedFrame = rule.mteTaggedFrame;
#endif
    cieInfo.returnAddressRegister = rule.returnAddressRegister;
    cieInfo.isSignalFrame = rule.isSignalFrame;
  }

  /// Which half of the rule a lookup copies out of the slot. Each caller reads
  /// only one half, and copying the whole slot was a measurable part of every
  /// frame:
  ///   - `kInfoPart`: the proc info, `startIP` to `gp`, which
  ///     `setInfoBasedOnIPRegister` puts into `unw_proc_info_t`.
  ///   - `kStepPart`: everything from `cfaRegister` on, which a step applies,
  ///     with `DwarfInstructions::applyCachedRule` or through `expandRule`.
  /// No caller needs both: when a step runs, the cursor already holds the proc
  /// info from its own lookup.
  enum Part { kInfoPart, kStepPart };

  static bool find(pint_t key, Rule &rule, Part part) {
    Slot &slot = slotFor(key);
    uint64_t seq = __atomic_load_n(&slot.seq, __ATOMIC_ACQUIRE);
    if (seq == 0 || (seq & 1))
      return false;
    // The key is the first word; most misses end here without a copy.
    if (__atomic_load_n(&slot.words[0], __ATOMIC_RELAXED) != (uint64_t)key)
      return false;
    const size_t first = part == kStepPart ? kStepFirstWord : kRuleFirstWord;
    const size_t last = part == kInfoPart ? kStepFirstWord : kWords;
    uint64_t words[kWords];
    for (size_t i = first; i < last; ++i)
      words[i] = __atomic_load_n(&slot.words[i], __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot.seq, __ATOMIC_RELAXED) != seq)
      return false;
    const size_t begin = first * 8;
    const size_t end = last == kWords ? sizeof(Entry) : last * 8;
    memcpy(reinterpret_cast<char *>(&rule) + (begin - kRuleOffset),
           reinterpret_cast<const char *>(words) + begin, end - begin);
    return true;
  }

  static void add(pint_t key, const Rule &rule) {
    Slot &slot = slotFor(key);
    uint64_t seq = __atomic_load_n(&slot.seq, __ATOMIC_RELAXED);
    if ((seq & 1) ||
        !__atomic_compare_exchange_n(&slot.seq, &seq, seq + 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint64_t words[kWords] = {};
    Entry entry = {key, rule};
    memcpy(words, &entry, sizeof(entry));
    for (size_t i = 0; i < kWords; ++i)
      __atomic_store_n(&slot.words[i], words[i], __ATOMIC_RELAXED);
    __atomic_store_n(&slot.seq, seq + 2, __ATOMIC_RELEASE);
  }

private:
  struct Entry {
    pint_t key;
    Rule rule;
  };

  static constexpr size_t kWords = (sizeof(Entry) + 7) / 8;
  static constexpr size_t kRuleOffset = offsetof(Entry, rule);
  static constexpr size_t kRuleFirstWord = kRuleOffset / 8;
  static constexpr size_t kStepFirstWord =
      (kRuleOffset + offsetof(Rule, cfaRegister)) / 8;
  static_assert(kRuleOffset % 8 == 0 &&
                    (kRuleOffset + offsetof(Rule, cfaRegister)) % 8 == 0,
                "a partial lookup copies whole words");
  static constexpr unsigned kSlotsLog2 = 12;

  /// `seq` is odd while the slot is being written and zero while it is empty.
  struct Slot {
    uint64_t seq;
    uint64_t words[kWords];
  };

  static Slot &slotFor(pint_t key) {
    // Fibonacci hashing: 2^64 / golden ratio (Knuth, TAOCP vol. 3, 6.4) spreads
    // clustered return addresses over the slots; the top bits index the table.
    return _slots[((uint64_t)key * 0x9E3779B97F4A7C15ULL) >> (64 - kSlotsLog2)];
  }

  static Slot _slots[1u << kSlotsLog2];
};

template <typename A>
typename DwarfRuleCache<A>::Slot
    DwarfRuleCache<A>::_slots[1u << DwarfRuleCache<A>::kSlotsLog2];

} // namespace libunwind

#endif // defined(_LIBUNWIND_USE_DWARF_RULE_CACHE)

#endif // __DWARF_RULE_CACHE_HPP__
