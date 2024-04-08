`CodeCache`由一个或多个`CodeHeap`组成，每个`CodeHeap`包含具有特定`CodeBlobType`类型的`CodeBlob`。

使用`-XX:+SegmentedCodeCache`可开启`CodeCache`分段，或当分层编译启用且`ReservedCodeCacheSize`超过240MB时默认启动，此时`CodeCache`分成三个区域（可通过`ReservedCodeCacheSize, NonProfiledCodeHeapSize, ProfiledCodeHeapSize`参数控制这三个区域的大小）：
- 非`nmethod(native method)`区域：包括`Buffers, Adapters, Runtime Stubs`
- 带`Profiling`信息的`nmethod`区域：处于分层编译的2和3级别的`nmethod`
- 不带`Profiling`信息的`nmethod`区域：处于分层编译的1和4级别的`nmethod`

`CodeCache`使用多个可变数组存储`CodeHeap`，每个数组按照其储存的`CodeHeap`的`CodeBlobType`顺序排列，如果`CodeBlobType`相同，则按照`CodeHeap`的地址排序。

如果开启分段，`CodeCache`的内存布局如下：
```
---------- high -----------
Non-profiled nmethods
Profiled nmethods
Non-nmethods
---------- low ------------
```

`CodeCache`的`allocate`方法有如下注释：
```
/**
 * Do not seize the CodeCache lock here--if the caller has not
 * already done so, we are going to lose bigtime, since the code
 * cache will contain a garbage CodeBlob until the caller can
 * run the constructor for the CodeBlob subclass he is busy
 * instantiating.
 */
```
即`allocate`方法需要由调用方负责获取锁，否则`CodeCahe`中可能会出现一个未初始化的`CodeBlob`。

`free`方法会更新一些计数器。
`commit`方法会更新计数器并flush指令缓存。