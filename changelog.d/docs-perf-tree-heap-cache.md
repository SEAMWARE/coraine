### Faster

- Batch writes with many clients: the tree heap (corTree) keeps a cache of free blocks per thread and size,
  so threads writing the store no longer wait for each other on one mutex a size. With 50 clients on corDB
  (durable): batch update (20) +79 %, batch create (20) +186 %, batch delete (20) +225 %, merge +8 %;
  single-entity writes and reads unchanged, RSS unchanged.
  ([Threads building trees at once](doc/performance.md#threads-building-trees-at-once))
