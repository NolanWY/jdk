# 解释器视角
编译流程：
1. `InterpreterGenerator::generate_counter_overflow`方法用于处理方法调用计数超过阈值的情形，此时会调用`InterpreterRuntime::frequency_counter_overflow`方法。
2. `InterpreterRuntime::frequency_counter_overflow`的`branch_bcp`参数表示调用计数超过阈值时循环跳转的地址，如果不是循环则为`NULL`。
3. 如果不是OSR，`InterpreterRuntime::frequency_counter_overflow`会提交编译任务并返回NULL；如果是OSR，该方法会返回编译完成的`nmethod`。

# 编译器视角
## C1编译器
compiler线程主循环:
```c++
void CompileBroker::compiler_thread_loop() {
  // 1. Init
  ...
  // 2. Main loop: Poll for new compilation tasks as long as the JVM runs.
  while (!is_compilation_disabled_forever()) {
    // 3. Retrieve a task from queue
    CompileTask* task = queue->get();
    // 4. Compile
    invoke_compiler_on_method(task);
  }
  // 5. Shut down compiler runtime
  shutdown_compiler_runtime(thread->compiler(), thread);
}
```

`CompileBroker::invoke_compiler_on_method`方法会调用对应compiler的`compile_method`方法进行编译，最后进入`Compilation::compile_method`方法开始编译：
```c++
void Compilation::compile_method() {
  // 1. setup compilation
  initialize();
  // 2. compile method
  int frame_size = compile_java_method();
  // 3. install code
  install_code(frame_size);
}
```

C1编译包含三个主要阶段，即HIR，LIR与发射阶段：
```c++
int Compilation::compile_java_method() {
  build_hir();
  emit_lir();
  return emit_code_body();
}
```

### HIR构建
```c++
void Compilation::build_hir() {
  _hir = new IR(this, method(), osr_bci());

  _hir->optimize_blocks();
  _hir->split_critical_edges();

  // compute block ordering for code generation
  // the control flow must not be changed from here on
  _hir->compute_code();

  GlobalValueNumbering gvn(_hir);
  RangeCheckElimination::eliminate(_hir);

  // loop invariant code motion reorders instructions and range
  // check elimination adds new instructions so do null check
  // elimination after.
  _hir->eliminate_null_checks();

  // compute use counts after global value numbering
  _hir->compute_use_counts();
}
```

1. 生成BlockList
   1. 建立start block和exception entry block
   2. 运行基本块算法，依次遍历字节码
      - 如果当前字节码可能触发异常，则添加从当前基本块到对应exception entry block的边
      - 如果当前字节码会修改本地变量，则在当前基本块中记录（基本块会有一个bitmap用于记录哪些变量被修改过，用于SSA中的phi函数处理）
      - 处理分支、跳转、返回等字节码
   3. 利用深度优先遍历标记循环
2. 使用SSA指令填充每个基本块
   1. 对基本块进行广度优先遍历（`GraphBuilder::iterate_all_blocks`方法）
      - 对基本块中的字节码进行抽象解释
      - 对解释执行构造出的SSA指令进行规范化（Canonicalize）处理，包括常量折叠、代数化简、重组合与局部值编号
      - 在处理invoke字节码时，会尝试进行方法内联
      - `GraphBuilder::append_with_bci`方法会将最终的SSA指令添加进当前基本块
    2. 消除冗余phi函数，如`x = [y, y]`
3. HIR优化
   1. 消除条件表达式`Optimizer::eliminate_conditional_expressions`：使用谓词执行（条件传送指令）替换条件表达式，在HIR阶段将If替换成IfOp
   2. 块合并`Optimizer::eliminate_blocks`
   3. 分割关键边：如果一个块有多个后继节点，并且其中一个后继节点有多个前驱节点，那么这个边就被认为是关键边。`split_edges`会在关键边的起始块和终止块之间插入一个新的块，从而分割这个关键边，以供后续编译优化使用。可用于在边上插入计算，而不会影响其他边。
   4. 计算用于代码生成的block order，从此时开始控制流不再发生变化
   5. 全局值编号
   6. 范围检测消除
   7. 空检测消除

### LIR构建