## Description

`SyncedChunkedArray` is thread-safe, unordered container, which allow iteration with thread-safe element access at `std::deque` / `std::vector` speed. Plus it have non-invalidatable iterators (`trackable_iterator`).

High iteration speed achieved by:

- elements stored continuously in chunks.
- You don't need to lock each element separately.
- Locked chunks are skipped during iteration. Skipped chunks iterates later.

Thread-safe element access works on per-chunk lock basis. Lock chunk - lock its all elements. This, may introduce some lock granularity, when accessing from multiple threads to elements from the same chunk (through `trackable_iterator`); you'll have sequenced access at worst case. Smaller chunks - means smaller granularity.

You can use it without thread-safe element access ability - just use `_shared` versions (`iterate_shared` / `trackable_iterator::lock_shared`).

You may call `emplace` /`erase` at any time, even right from the iteration loop. Have roughly O(1) complexity.

## How to use

You need `SyncedChunkedArray.h` and `threading` folder. Elements must be movable.
Example:

```C++
#include "SyncedChunkedArray.h"

int main(){
  SyncedChunkedArray<int> list;
  for(int i=0;i<4000;i++)list.emplace(i);

  SyncedChunkedArray<int>::trackable_iterator two_iter = list.emplace(2)();
  SyncedChunkedArray<int>::trackable_iterator last_iterated;
  
  auto fn = [&](){
    list.iterate([&](auto iter){
      if (*iter > 500){
        list.erase(iter);
      } else {
        (*iter)++;                  // element under unique_lock, we can safely mutate it.
      }
      last_iterated = {iter};		// carefull, trackable_iterator move is relativley costly.
    });
  };
  
  std::thread t1(fn);
  std::thread t2(fn);
  t1.join();
  t2.join();
  
  std::cout << *two_iter.lock() << std::endl;        // Output: 4
  std::cout << *last_iterated.lock() << std::endl;   // May be any number, iteration is unordered
  
  return 0;
}
```

`SyncedChunkedArray` have:

* emplace(args...)  - in-place construct element in container. Return lambda. Lambda return `trackable_iterator`. Do not store lambda. `empalce` does not lock/block.

* erase(Iterator) - mark element as erased. If `SyncedChunkedArray::erase_immideatley` is true, tries lock chunk, and maintain, thus destroy element immediately.

* erase(trackable_iterator) - same as `erase(Iterator)`

* iterate - executes closure with `Iterator` as parameter. Lock each chunk with exclusive(write) lock.

* iterate_shared - same as `iterate`, but chunks locked with shared (read) lock.

  ​

`SyncedChunkedArray<T>::trackable_iterator ` have:

* lock() - Lock elements chunk. return `access` to element. Chunk unlocked on `access` destruction.
* lock_shared() - same as `lock()`, but use shared_lock.
* try_lock() - under construction
* try_lock_shared() - under construction

## Structure

SyncedChunkedArray is a deque-like container. It consists from `Chunk`s of fixed size. 

Target `Chunk` size is 4Kb. Higher size does not affect performance at i7-4771 (may be because 4Kb - is L1 data cache size). Minimal is 32; at smaller sizes, chunk lock/unlock overhead becomes observable.

 `Chunk` looks like:

```C++
struct Chunk{
    Lock lock;

    int size;
    bool alive[chunk_size];
    T data[chunk_size];    
}
```

Lock is recursive RWSpinLock with level counter (we maintain only at level 1). Recursive is only lock part. See implementation for details.

Recursivity needed for cases when we need to lock a few elements from the same chunk simultaneously.

RWSpinLock needed for `iterate_shared()`,  `trackable_iterator::lock_shared()`. Otherwise SpinLock will be fine.

## Iteration

During iteration we try to lock each chunk, if we fail to lock - store that chunk, and skip it.
After we lock chunk, we iterate all chunk data, skipping not alive. At the end, we do chunk maintance procedure, and unlock chunk.

```C++
std::vector<Chunk*> skipped;
chunk = first;
while(chunk){
    if (chunk->lock.try_lock()){
        for(int i=0;i<chunk->size;i++){
            if (!alive[i]) continue;
            closure(chunk->data[i]);
        }
        maintain(chunk);
        chunk->lock.unlock();
    } else {
        skipped.emplace_back(chunk);
    }
    chunk = chunk->next;
}
```

Then we loop `skipped` Chunks until lock and iterate all.

Thus, we can iterate from multiple threads, without need to lock each element separately. And we can erase/emplace during iteration.

## Maintance

Maintance may occur only on exclusively locked `Chunk::lock`.

Maintance include:

 * compacting chunk
 * merging adjacence chunks
 * deleting empty chunk

If during maintance chunk was not destroyed, and it is not full - we add it to `free_list` (we use it in `emplace()`).

#### Deleting

If chunk have no alive elements - we delete it.

#### Compacting

If chunk have deleted elements (!alive), we destroy them, and fill gaps with elemenets from the `Chunk::data` end. In other words, make `data` linear.

#### Merging

If chunk have very small amount of alive elements, we try to merge chunk with `prev`/`next`.

For merge we must lock other chunk. If we fail to lock, we do not merge.

During merge, we simply move all elements from one chunk to another. And destroy source chunk.


## Erase

On erase we set alive[index] flag to true, then (optionally) try to maintain chunk.

## Emplace

We try to reuse not full chunks (from `free_list`), before we create new one. `emplace()` return `trackable_iterator`.


## trackable_iterator

`trackable_iterator` is somewhat similar to weak_ptr. It have `lock()`, which allow you to access element under chunk lock, and prevent chunk maintance (element does not move).

You can get it from `emplace()`, and construct from `Iterator` (during `iterate()`).

When maintance occurs, element may be moved. If element have trackable_iterators, they all will be updated with its new address (actually chunk/index).

Price to call `trackable_iterator.lock()` is similar to `weak_ptr.lock()`. But unlike `weak_ptr.lock()` which only guarantee object aliveness, `trackable_iterator` also provide object thread-safety.

P.S. If you use have many trackable_iterators pointing to the same container element, it is, probably, better to use one `std::shared_ptr<trackable_iterator>` instead; because move of element in container, will cause update of all trackable_iterators pointing to it.


---

# N.B.

* `SyncedChunkedArray` move now may be not-thread safe (I'm not sure).
* `~SyncedChunkedArray()` will block, untill all iterations finishes, and all trackable_iterators unlocks.


* As you can see, it is theoretically possible to have ordered `SyncedChunkedArray`. With only `emplace_back()`, `erase`, `iterate` operations. compact/merge operation will cost higher, all other the same. Let me know, if you have need in this.
