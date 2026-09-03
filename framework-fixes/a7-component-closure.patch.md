# CNA-FINDINGS A7 — widening the sharp-runtime component closure is undiscoverable

Not a patch, because there is nothing to fix in the code: the mechanism works exactly as designed.
What is missing is that nobody can find it, and the failure it produces points somewhere else.

## What happens

sharp-runtime instantiates a component only when something asks for it. A consumer that reaches for
`System::Threading::Tasks::Parallel` without naming it gets no `SharpRuntime::Threading.Tasks`
target, so the include path does not propagate, and the error is a missing header from deep inside
CNA's own configure — a file the consumer never mentioned, in a project it did not write.

The fix is one line, and it has to come *before* `add_subdirectory(../cnanext)`:

```cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/../cnanext/cmake/SharpRuntimeConsumption.cmake")
set(SHARP_RUNTIME_COMPONENTS "${CNA_SHARP_RUNTIME_DEFAULT_COMPONENTS};Threading.Tasks;Diagnostics"
    CACHE STRING "Sharp Runtime components required by this consumer")
```

Finding that took reading `cmake/SharpRuntimeConsumption.cmake` in CNA and working backwards from
the variable name. Nothing in the error mentions components, closures, or that file.

## Suggested, in decreasing order of value

1. **A diagnostic at configure time.** `SharpRuntimeConsumption.cmake` knows the default closure.
   When a consumer's compile fails on a header from a component that exists but was not requested,
   the closure is the first thing to check — and CMake is the only place that can say so. A
   `message(STATUS "sharp-runtime components: ${SHARP_RUNTIME_COMPONENTS}")` at configure time
   would put the answer in every build log, next to the question.
2. **The line above, in CNA's own README**, in the section a consumer reads to add CNA to a
   project. It is three lines of CMake and it is the difference between a working build and an
   afternoon.
3. Nothing else. The design is right: a closure that instantiated everything would make every
   consumer pay for every component, which is the thing this avoids.

## Why this is a note rather than a patch

Both suggestions are in CNA's own build and documentation rather than in its code, and both are
choices about how CNA presents itself to consumers — which is the maintainer's call, not something
a consumer should silently patch in.
