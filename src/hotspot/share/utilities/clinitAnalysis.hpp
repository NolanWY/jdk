#ifndef SHARE_VM_UTILITIES_CLINIT_ANALYSIS_HPP
#define SHARE_VM_UTILITIES_CLINIT_ANALYSIS_HPP

#include "interpreter/bytecodeStream.hpp"
#include "memory/allocation.hpp"
#include "runtime/handles.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/resourceHash.hpp"

// ClinitAnalysis is a class that performs a static analysis of the class initialization order.
// Currently, it only analyzes classes that have initializer and are already initialized.
class ClinitAnalysis : KlassClosure {
 public:
  // Run the clinit analysis and return the initialization sequence.
  static GrowableArray<InstanceKlass *>* run(TRAPS);

 private:
  Thread *THREAD;
  GrowableArray<InstanceKlass *> *_klass_initialization_sequence;
  ResourceHashSet<Method *> *_visited_methods;
  ResourceHashSet<InstanceKlass *> *_visited_klasses;

  ClinitAnalysis(Thread *thread);
  void analyze();
  void do_klass(Klass *klass);
  void analyze(InstanceKlass *instanceKlass);
  void analyze_super_interfaces(InstanceKlass *instanceKlass);
  void analyze_initializer(InstanceKlass *instanceKlass);
  void analyze_method(Method *method);
  void handle_get_put(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream);
  void handle_invoke_static_special(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream);
  void handle_new(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream);
  void handle_invoke_virtual_interface(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream);
  void resolve_virtual_call(GrowableArray<Method *> &resolvedMethods, Klass *baseKlass, Symbol *name, Symbol *signature);
};

#endif//SHARE_VM_UTILITIES_CLINIT_ANALYSIS_HPP
