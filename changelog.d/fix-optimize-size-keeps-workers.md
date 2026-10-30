### Changed behaviour

- A `COR_OPTIMIZE=size` build keeps the worker pool of a speed build (32 with mongoc) and changes only the malloc arena: with mongoc on 4 CPUs a worker per CPU saved another 1.5 MB after 100 000 entities and cost 34-46 % of the requests at 50 connections; one arena alone costs 0-3 % with mongoc, and with corDB up to 17 % of the creates (doc/building-details.md#cor_optimize)

### Fixed

- The nightly's ETSI jobs passed `COR_OPTIMIZE=` empty (their matrix has no optimize), which CMake refuses: they would not have built. The makefile now refuses an empty `COR_OPTIMIZE` itself, and the valgrind size leg is gone - under valgrind malloc is valgrind's, so it ran the speed broker
