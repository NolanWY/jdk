`CodeHeap`的重要字段：
- `_memory`：`CodeHeap`管理的内存区域
- `_segmap`：用于记录`segments`的分配情况，`0xFF`表示空闲。主要用于帮助确定一个给定的内存地址属于哪个`HeapBlock`
- `_next_segment`：记录下一个可分配的段（即当前地址最小的不被任何block所描述的段）
- `_free_list`：空闲的`HeapBlock`链表，地址从小到大增加

注意`_segmap`的含义不是记录某个段是否被`allocate`方法分配出去了，最新版本中的解释如下：
```c++
// The segmap is marked free for that part of the heap
// which has not been allocated yet (beyond _next_segment).
// The range of segments to be marked is given by [beg..end).
// "Allocated" space in this context means there exists a
// HeapBlock or a FreeBlock describing this space.
// This method takes segment map indices as range boundaries

// Don't get confused here.
// All existing blocks, no matter if they are used() or free(),
// have their segmap marked as used. This allows to find the
// block header (HeapBlock or FreeBlock) for any pointer
// within the allocated range (upper limit: _next_segment).
```

注意`reserved`方法是如何计算`_segmap`的大小的：
```c++
_number_of_committed_segments = size_to_segments(_memory.committed_size());   // 已提交的段的数量
_number_of_reserved_segments  = size_to_segments(_memory.reserved_size());    // 已保留的段的数量
const size_t reserved_segments_alignment = MAX2((size_t)os::vm_page_size(), granularity);
const size_t reserved_segments_size = align_up(_number_of_reserved_segments, reserved_segments_alignment);  // 一个segment占在_segmap中占一字节
const size_t committed_segments_size = align_to_page_size(_number_of_committed_segments);
// reserve space for _segmap
if (!_segmap.initialize(reserved_segments_size, committed_segments_size)) {   // reserved_segments_size会被隐式构造为一个ReservedSpace，
  return false;                                                               // 由于一个segment占在_segmap中占一字节，因此段的数量即为已提交的字节数
}
```

通过阅读`mark_segmap_as_used`方法可以得知`_segmap`如何标记正在被使用的段：
```c++
void CodeHeap::mark_segmap_as_used(size_t beg, size_t end) {
  address p = (address)_segmap.low() + beg;
  address q = (address)_segmap.low() + end;
  int i = 0;
  while (p < q) {
    *p++ = i++;
    if (i == free_sentinel) i = 1;
  }
}
```
即从0开始递增编号，跳过0xFF，然后重复。

在分配内存时，
- Fast path: 
  1. 从`free_list`中查找第一个大小符合要求的的`HeapBlock`。
  2. 如果该`HeapBlock`剩余空间不足`CodeCacheMinBlockLength`，则直接将该block从链表中摘下。
  3. 否则对该块进行切分，并返回后面的块，同时更新`segmap`（因为有一个新的block来描述这一块）。
- Slow path:
  1. 如果没有则会从`_next_segment`开始分配，同时更新`_segmap`