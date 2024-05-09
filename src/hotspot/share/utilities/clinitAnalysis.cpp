#include "utilities/clinitAnalysis.hpp"
#include "classfile/classLoader.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/fieldDescriptor.hpp"
#include "utilities/exceptions.hpp"

ClinitAnalysis::ClinitAnalysis(Thread *thread)
    : _visited_klasses(new ResourceHashSet<InstanceKlass *>),
      _visited_methods(new ResourceHashSet<Method *>()),
      _klass_initialization_sequence(new GrowableArray<InstanceKlass *>()) {
  THREAD = thread;
}

GrowableArray<InstanceKlass *> *ClinitAnalysis::run(TRAPS) {
  tty->print("Starting clinit analysis\n");
  ClinitAnalysis clinitAnalysis(THREAD);
  clinitAnalysis.analyze();
  tty->print("Clinit Analysis Result:\n");
  for (int i = 0; i < clinitAnalysis._klass_initialization_sequence->length(); i++) {
    tty->print("%d %s\n", i, clinitAnalysis._klass_initialization_sequence->at(i)->name()->as_C_string());
  }
  return clinitAnalysis._klass_initialization_sequence;
}

void ClinitAnalysis::analyze() {
  ClassLoaderDataGraph::loaded_classes_do(this);
}

void ClinitAnalysis::do_klass(Klass *klass) {
  // Filter out non-InstanceKlass
  if (klass->is_instance_klass()) {
    analyze(InstanceKlass::cast(klass));
  }
}

void ClinitAnalysis::analyze(InstanceKlass *instanceKlass) {
  // Filter out non-initialized classes and already visited classes
  if (!instanceKlass->is_initialized() || !_visited_klasses->put(instanceKlass)) {
    return;
  }
  tty->print("Analyzing class %s\n", instanceKlass->name()->as_C_string());

  // Java Virtual Machine Specification, Chapter 5.5, Initialization
  if (!instanceKlass->is_interface()) {
    // Initialize super class first
    if (instanceKlass->superklass() != NULL) {
      analyze(instanceKlass->superklass());
    }
    // Analyze superinterfaces (whether direct or indirect) that declare at least one non-abstract, non-static method.
    // Check if the class has non-static concrete methods, if not,
    // then none of the superinterfaces has any non-static concrete methods.
    if (instanceKlass->has_nonstatic_concrete_methods()) {
      analyze_super_interfaces(instanceKlass);
    }
  }
  // Time for current class to be initialized
  analyze_initializer(instanceKlass);
}

void ClinitAnalysis::analyze_super_interfaces(InstanceKlass *instanceKlass) {
  Array<Klass *> *interfaces = instanceKlass->local_interfaces();
  for (int i = 0; i < interfaces->length(); i++) {
    InstanceKlass *iface = InstanceKlass::cast(interfaces->at(i));
    // Filter out already visited classes
    if (!_visited_klasses->put(iface)) {
      continue;
    }
    // Analyze superinterfaces that declare at least one non-abstract, non-static method.
    if (iface->has_nonstatic_concrete_methods()) {
      analyze_super_interfaces(iface);
    }
    // If the interface declares non-static concrete methods, initialize it
    if (iface->declares_nonstatic_concrete_methods()) {
      analyze_initializer(iface);
    }
  }
}

void ClinitAnalysis::analyze_initializer(InstanceKlass *instanceKlass) {
  // Filter out classes without class initializer, since we do not need to reinitialize them.
  Method *initializer = instanceKlass->class_initializer();
  if (initializer == NULL) {
    return;
  }
  analyze_method(initializer);
  _klass_initialization_sequence->push(instanceKlass);
}

void ClinitAnalysis::analyze_method(Method *method) {
  // Filter out methods in non-initialized classes and already visited methods
  if (!method->method_holder()->is_initialized() || !_visited_methods->put(method)) {
    return;
  }
  tty->print("Analyzing method %s\n", method->name_and_sig_as_C_string());
  methodHandle methodHandle(THREAD, method);
  constantPoolHandle poolHandle(methodHandle()->constants());
  BytecodeStream bytecodeStream(methodHandle);
  Bytecodes::Code code;
  while ((code = bytecodeStream.next()) >= 0) {
    switch (code) {
      case Bytecodes::_new:
        handle_new(poolHandle, &bytecodeStream);
        break;
      case Bytecodes::_getstatic:
      case Bytecodes::_putstatic:
      case Bytecodes::_getfield:
      case Bytecodes::_putfield:
        handle_get_put(poolHandle, &bytecodeStream);
        break;
      case Bytecodes::_invokestatic:
      case Bytecodes::_invokespecial:
        handle_invoke_static_special(poolHandle, &bytecodeStream);
        break;
      case Bytecodes::_invokevirtual:
      case Bytecodes::_invokeinterface:
        handle_invoke_virtual_interface(poolHandle, &bytecodeStream);
        break;
      default: break;
    }
  }
}

void ClinitAnalysis::handle_get_put(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream) {
  // Index of getstatic and putstatic will be rewritten
  int index = bytecodeStream->get_index_u2_cpcache();
  Klass *targetKlass = poolHandle->klass_ref_at(index, CHECK);
  analyze(InstanceKlass::cast(targetKlass));
}

void ClinitAnalysis::handle_invoke_static_special(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream) {
  // Index of invokestatic and invokespecial will be rewritten
  Bytecode_invoke invokeCode = Bytecode_invoke(bytecodeStream->method(), bytecodeStream->bci());
  methodHandle targetMethod = invokeCode.static_target(CHECK);
  InstanceKlass *targetKlass = targetMethod->method_holder();
  // Invokestatic will trigger class initialization
  analyze(targetKlass);
  // Step into the target method
  analyze_method(targetMethod());
}

void ClinitAnalysis::handle_new(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream) {
  int index = bytecodeStream->get_index_u2();
  Klass *targetKlass = poolHandle->klass_at(index, CHECK);
  analyze(InstanceKlass::cast(targetKlass));
}

void ClinitAnalysis::handle_invoke_virtual_interface(constantPoolHandle poolHandle, BytecodeStream *bytecodeStream) {
  // Index of invokevirtual and invokeinterface will be rewritten
  int index = bytecodeStream->get_index_u2_cpcache();
  Klass *baseKlass = poolHandle->klass_ref_at(index, CHECK);
  Symbol *methodName = poolHandle->name_ref_at(index);
  Symbol *methodSig = poolHandle->signature_ref_at(index);
  GrowableArray<Method*> resolvedMethods;
  resolve_virtual_call(resolvedMethods, baseKlass, methodName, methodSig);
  for (int i = 0; i < resolvedMethods.length(); i++) {
    methodHandle method = resolvedMethods.at(i);
    analyze(method->method_holder());
    analyze_method(method());
  }
}

void ClinitAnalysis::resolve_virtual_call(GrowableArray<Method *> &resolvedMethods, Klass *baseKlass, Symbol *name, Symbol *signature) {
  {
    LinkInfo linkInfo(baseKlass, name, signature);
    Method *targetMethod = LinkResolver::lookup_method_in_klasses(linkInfo, true, false);
    if (targetMethod != NULL) {
      resolvedMethods.append(targetMethod);
    }
  }

  Klass *firstChildKlass = baseKlass->subklass();
  while (firstChildKlass != NULL) {
    Klass *curKlass = firstChildKlass;
    while (curKlass != NULL) {
      LinkInfo linkInfo(curKlass, name, signature);
      Method *targetMethod = LinkResolver::lookup_method_in_klasses(linkInfo, true, false);
      if (targetMethod != NULL) {
        resolvedMethods.append(targetMethod);
      }
      curKlass = curKlass->next_sibling();
    }
    firstChildKlass = firstChildKlass->subklass();
  }
}
