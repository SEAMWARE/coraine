### Less memory

- Every thread of the broker used 256 KiB of RAM for a bridge sample buffer that only a bridge plugin's
  threads need - 18 MB with mongoc's 69 threads, before a single request. The buffer is now allocated by
  a thread at its first sample.
