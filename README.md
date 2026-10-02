# osmesa-src
OSMesa source code and cargo build scripts to compile on Linux and Mac

The bundled source is Mesa 25.0.7. Mesa 25.1 removed OSMesa, so the update
script deliberately uses the final 25.0 release instead of tracking Mesa main.
Run `tools/update-mesa.sh` to reproduce the source import from the release
archive; the script verifies its SHA-256 checksum.

Building requires Meson 1.1 or newer, Ninja, Python 3 with Mako and PyYAML,
Flex, Bison, a C++17 compiler, and LLVM development libraries.
