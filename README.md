# arm64-vmem-stress

Reproducer for random allocation failures seen when many mariadbd processes
run on Windows ARM64 (GitHub `windows-11-arm`) with a huge `MEM_RESERVE`.

Each process reserves `--reserve-gb` of address space, commits `--chunk-mb`
at the start of it once (like the InnoDB buffer pool), and then runs
malloc/free churn. On the first failure it dumps system and process memory
state. Run the `vmem-stress` workflow manually.
