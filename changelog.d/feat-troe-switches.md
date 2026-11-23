### New

- Build switches for the temporal history: `COR_FEATURE_TROE_CORDB` (corDB's own history, `--troe corDB`) and `COR_FEATURE_TROE_TIMESCALE` (`timescale.so`), both on by default - `coraine-build --features TROE_CORDB=OFF,TROE_TIMESCALE=OFF` builds a broker that keeps no history of its own (`--troe none`), for a deployment that keeps it elsewhere by subscription (QuantumLeap, Cygnus, Kafnus). A broker built without one does not offer it as a `--troe` value.
