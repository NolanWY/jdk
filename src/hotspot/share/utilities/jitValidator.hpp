#ifndef JDK_JITVALIDATOR_HPP
#define JDK_JITVALIDATOR_HPP

#include "code/nmethod.hpp"
#include "memory/iterator.hpp"
#include "utilities/Zydis.h"
#include "utilities/exceptions.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/pair.hpp"

#define JIT_VALIDATOR_DETAIL_LOG(format, ...) \
  do {                                        \
    if (_log_level == DETAIL) {               \
      tty->print(format, ##__VA_ARGS__);      \
    }                                         \
  } while (0)

// Provide convenient method to get the memory flag of a given address.
// -XX:NativeMemoryTracking=[summary|detail] should be set when using this class.
class MemoryRegionLocator : public VirtualMemoryWalker {
 public:
  MemoryRegionLocator() : target(NULL), flag(mt_number_of_types) {}

  bool do_allocation_site(const ReservedMemoryRegion *rgn) {
    if (rgn->contain_address(target)) {
      flag = rgn->flag();
      return false;
    }
    return true;
  }

  // Get the memory flag of the given address.
  // Return true if the locator successfully find the memory flag of the given address,
  // and `result` will be set to the memory flag of the given address.
  bool get_flag(address p, MEMFLAGS &result) {
    target = p;
    // When walk_virtual_memory return false, it means that we successfully find a matched
    // virtual memory space and return in advance.
    if (!VirtualMemoryTracker::walk_virtual_memory(this)) {
      result = flag;
      return true;
    }
    return false;
  }

 private:
  // The target address we want to locate.
  address target;

  // The memory flag of the target address.
  MEMFLAGS flag;
};

// -XX:NativeMemoryTracking=[summary|detail] should be set when using JitValidator.
// Log level of JitValidator can be set with -XX:JitValidatorLogLevel=[off|summary|detail]
class JitValidator : CodeBlobClosure {
 public:
  static void run();

 private:
  ZydisDecoder _decoder;
  ZydisFormatter _formatter;
  MemoryRegionLocator _address_locator;

  // Statistics
  GrowableArray<address> *_recognizable_address[mt_number_of_types];
  GrowableArray<address> *_unrecognizable_address;
  int _nmethod_count;
  enum { OFF,
         SUMMARY,
         DETAIL } _log_level;

  JitValidator();
  void do_code_blob(CodeBlob *cb);
  void analyze();
  void analyze_nmethod(nmethod *nm);

  void print_instruction(const ZydisDecodedInstruction *instruction,
                         const ZydisDecodedOperand operands[],
                         ZyanU64 runtimeAddress);
  void print_statistics();

  struct OperandValue {
    enum { INVALID = 0, UNSIGNED, SIGNED } type;
    union { u8 u; s8 s; } value;
  };

  OperandValue get_imm_operand(const ZydisDecodedInstruction *instruction,
                               const ZydisDecodedOperand *operand,
                               ZyanU64 runtimeAddress);
  OperandValue get_mem_operand(const ZydisDecodedInstruction *instruction,
                               const ZydisDecodedOperand *operand,
                               ZyanU64 runtimeAddress);
  OperandValue calculate_absolute_address(const ZydisDecodedInstruction *instruction,
                                          const ZydisDecodedOperand *operand,
                                          ZyanU64 runtimeAddress);

  void handle_value(OperandValue value);
};

class KlassCounter : StackObj {
 public:
  void increase_count(Klass *klass) {
    for (int i = 0; i < klassCount.length(); i++) {
      Pair<Klass *, int> &entry = klassCount.at(i);
      if (entry.first == klass) {
        entry.second++;
        return;
      }
    }
    Pair<Klass *, int> entry(klass, 1);
    klassCount.append(entry);
  }

  void print_statistics() {
    klassCount.sort(compare);
    for (int i = 0; i < klassCount.length(); i++) {
      const Pair<Klass *, int> &entry = klassCount.at(i);
      tty->print("%s : %d\n", entry.first->signature_name(), entry.second);
    }
  }

 private:
  GrowableArray<Pair<Klass *, int> > klassCount;

  static int compare(Pair<Klass *, int> *pair1, Pair<Klass *, int> *pair2) {
    return pair2->second - pair1->second;
  }
};

#endif//JDK_JITVALIDATOR_HPP
