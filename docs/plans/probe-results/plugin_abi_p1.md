# Plugin ABI P1: two-function compiler round-trip (2026-11-16)

**Scope.** Linux x86-64, GCC 15.2.0 and Clang 21.1.8. A C plugin exports a table containing
its size and two function pointers. A C++ host built independently with the other compiler loads
the shared object with `dlopen`, finds its entry point, checks the table size, calls both functions,
and unloads it. The sources are in `tests/plugin_host/probe/`.

**Result.** GCC host / Clang plugin passed; Clang host / GCC plugin passed. The calls returned the
expected sum and product in both directions. This is a narrow calling-convention and struct-layout
probe, not a test of the full Apex SDK, plugin tools, or a guarantee for other architectures.

**Commands.** `scripts/build.sh linux-gcc-debug --target apex_plugin_abi_probe` and
`scripts/test.sh linux-gcc-debug -R '^plugin_abi_' --output-on-failure` (2/2 passed).

**Not tested.** Windows MSVC / clang-cl, and full Apex plugin/tool interaction across compilers.
Those remain required before treating the full P1 exit probe as complete.
