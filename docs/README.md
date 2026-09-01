# IndustrialBus implementation documentation

This directory describes the repository as it is implemented. The development
guide remains the design baseline; these documents record concrete module
ownership, target layout, runtime lifecycles, and test coverage.

- [Module responsibilities](modules.md)
- [Runtime lifecycle](runtime-lifecycle.md)
- [Testing](testing.md)

Documentation is part of the definition of done. A change that moves a
responsibility, adds a public option or wire message, changes a thread/process
lifecycle, changes a CMake target, or adds/removes coverage must update the
corresponding document in the same commit.
