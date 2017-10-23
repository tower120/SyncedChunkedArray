## Structure

SyncedChunkedArray is a deque-like container. It consists from `Chunk`s of fixed size.

 `Chunk` looks like:

```
struct Chunk{
    Lock lock;

    int size;
    bool alive[chunk_size];
    T data[chunk_size];    
}
```

Lock is recursive RWSpinLock with level counter (we maintain only at level 1). Recursive is only lock part. See implementation for details.

## Iteration

During iteration we try to lock each chunk, if we fail to lock - store that chunk, and skip it.
After we lock chunk, we iterate all chunk data, skipping not alive. At the end, we do chunk maintance procedure, and unlock chunk.

```
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

maintance include:
 * compacting chunk
 * merging adjacence chunks
 * deleting empty chunk

If during maintance chunk was not destroyed, and it is not full - we add it to `free_list` (we use it in emplace).

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

You can get it from `emplace()` and construct from `Iterator` (during `iterate()`).

When maintance occurs, element may be moved. If element have trackable_iterators, they all updated with it new address (actually chunk/index).

Price to call `trackable_iterator.lock()` is similar to `weak_ptr.lock()`. But unlike `weak_ptr.lock()` which only guarantee object aliveness, `trackable_iterator` also provide object thread-safety.