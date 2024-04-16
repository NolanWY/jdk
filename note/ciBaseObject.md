`ciBaseObject`用于储存用于操作oop的句柄（handle），即通过一次间接跳转访问实际的oop。
使用句柄访问oop可以使编译模块与垃圾回收模块解耦，即使垃圾回收模块移动了对象的位置，通过句柄也可以访问到实际的对象。

```c++
// Each instance of ciBaseObject holds a handle
// to a corresponding oop on the VM side and provides routines
// for accessing the information in its oop.  By using the ciBaseObject
// hierarchy for accessing oops in the VM, the compiler ensures
// that it is safe with respect to garbage collection; that is,
// GC and compilation can proceed independently without
// interference.
```

`ciBaseObject`的继承结构不再使用kclass/oop二分模型，而是将ciKclass放入ciMetadata中。