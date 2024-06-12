#ifndef JDK_JITVALIDATOR_HPP
#define JDK_JITVALIDATOR_HPP

#include "code/nmethod.hpp"
#include "memory/iterator.hpp"
#include "utilities/Zydis.h"
#include "utilities/exceptions.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/pair.hpp"
#include "utilities/resourceHash.hpp"

#define JIT_VALIDATOR_DETAIL_LOG(format, ...) \
  do {                                        \
    if (logLevel == DETAIL) {                 \
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

class AddressManager : StackObj {
 public:
  AddressManager();
  void add_recognizable_address(address p, nmethod *holder, MEMFLAGS flag);
  void add_unrecognizable_address(address p, nmethod *holder);

  void print_summary_statistics();
  void print_heap_address_statistics();

 private:
  GrowableArray<address> *_recognizable_addresses[mt_number_of_types];
  GrowableArray<address> *_unrecognizable_addresses;
  ResourceHashtable<nmethod *, GrowableArray<address> *> *_nmethod_to_addresses;
  ResourceHashtable<Klass *, GrowableArray<address> *> *_klass_to_heap_addresses;
  ResourceHashtable<Klass *, GrowableArray<nmethod *> *> *_klass_to_nmethods;

  void add_address_common(address p, nmethod *holder);

  class KlassSorter : StackObj {
   public:
    bool do_entry(Klass *const & key, GrowableArray<address> *const & value) {
      _klassCount.append(Pair<Klass*, int>(key, value->length()));
      return true;
    }

    GrowableArray<Klass *> *get_sorted_klass() {
      _klassCount.sort(compare);
      GrowableArray<Klass*> *klasses = new GrowableArray<Klass*>(_klassCount.length());
      for (int i = 0; i < _klassCount.length(); i++) {
        klasses->append(_klassCount.at(i).first);
      }
      return klasses;
    }

   private:
    GrowableArray<Pair<Klass*, int> > _klassCount;

    static int compare(Pair<Klass *, int> *pair1, Pair<Klass *, int> *pair2) {
      return pair2->second - pair1->second;
    }
  };
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
  AddressManager _address_manager;

  // Statistics
  int _nmethod_count;

  JitValidator();
  void do_code_blob(CodeBlob *cb);
  void analyze();
  void analyze_nmethod(nmethod *nm);

  void print_instruction(const ZydisDecodedInstruction *instruction,
                         const ZydisDecodedOperand operands[],
                         ZyanU64 runtimeAddress);
  void print_statistics();

  struct OperandValue {
    enum { INVALID = 0,
           UNSIGNED,
           SIGNED } type;
    union {
      u8 u;
      s8 s;
    } value;
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

  void handle_value(OperandValue value, nmethod *holder);
};

#endif//JDK_JITVALIDATOR_HPP
