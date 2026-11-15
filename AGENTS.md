# Build resource limits

The user reported the computer became unusable during earlier builds. Preserve
the parent directory's engineering standards and additionally follow these rules:

- Run at most one build, compiler, linter, or test workload at a time, including
  across subagents. Use one job (`cmake --build ... --parallel 1`, `make -j 1`,
  and serial tests). Do not launch several builds in the background.
- On Linux, lower CPU and I/O priority for builds and other expensive checks when
  available, for example `nice -n 15 ionice -c 3 cmake --build ... --parallel 1`.
- Check current memory and system load before expensive work. Stop or defer the
  workload if the machine is under pressure; avoid introducing swap or changing
  system settings. Do not claim the limits guarantee responsiveness.
- Tell subagents about these limits before delegating. Read-only reviews may run
  concurrently, but they must not start builds, tests, or services on their own.
