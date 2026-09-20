Context: pure C23 inference engine / runtime core.

Priorities:
1. correctness
2. performance (cache locality, low memory, predictable behavior)
3. portability
4. testability

Hard constraints:
- Use modern C23 features where they improve safety, clarity, or compile-time checking.
- Minimize naked pointer usage in public APIs. Count parameters precede the array/buffer they constrain.
  Prefer signatures like:
  - bool fn(size_t n, const float xs[static n]);
  - error_code fn(size_t dst_n, float dst[static dst_n], size_t src_n, const float src[static src_n]);
- Caller-provided buffers for I/O and scratch; make workspace requirements explicit.
- Hot paths must be allocation-free. No hidden heap allocations, copies, conversions, or reallocations.
- No recursion, no global mutable state, no hidden ownership, no undocumented aliasing.
- No undefined-behavior tricks, no silent truncation.
- No macro-heavy fake generics unless truly justified.
- No parser/server concerns unless explicitly requested.
- Use restrict only when semantically correct.
- Use attributes ([[nodiscard]], etc.) conservatively but meaningfully.

Memory allocation rule:
- geistshell does not allocate. The default is caller-provided buffers and explicit
  workspace/scratch memory, sized by a compile-time constant — that is what makes the
  spine replayable and portable to constrained targets.
- The engine's `heap.h` is NOT available to us. It lives in libgeist's private
  `src/base/`, and the `-I$(GEIST_DIR)` that once reached it was dropped with the arena
  wrapper in v0.3.1 (see the Makefile): geistshell depends on the engine's PUBLIC headers
  only. Do not reintroduce that include path, and do not maintain a local copy of it.
- Do not call `malloc`, `calloc`, `realloc`, or `free` unless there is a clear reason
  that is written down at the call site. Today two places in the core qualify, and both
  are sized by something outside this program: the macOS process table, whose size only
  the kernel knows (`src/machine/backend_macos.c`), and the geistd adapter's
  vocabulary-sized buffers, whose size the served model reports at open time
  (`src/model/model_adapter.c`). File slurping in the CLI surface is the third.
- Never allocate in a hot path, and never hide an allocation inside one.
- Where an allocation is unavoidable, prefer freeing it in the same function that
  made it. If ownership transfers to the caller instead, say so in the declaration's
  comment and name who must release it.

Inference-engine architecture:
- Separate clearly: model representation, immutable weights, runtime context, scratch memory, kernels, scheduling, token/state handling.
- Keep setup/loading separate from inference hot path.
- Distinguish: element count vs. byte count, shape vs. stride, capacity vs. used length.
- Prefer small, composable kernels with simple, predictable control flow.
- Keep data layouts and loops cache-friendly and compiler-visible; avoid abstraction layers that hurt optimization visibility.
- Numerically stable implementations for all reductions (softmax, layernorm, RMSNorm). Document precision assumptions.
- Isolate platform-specific code (NEON, SSE/AVX, CUDA) behind clear kernel boundaries.
- SIMD kernels must have scalar fallbacks. Document alignment requirements for buffers.
- Quantization: document accuracy tolerances and validate against reference implementations.

Error handling:
- Explicit error codes or tagged result structs.
- Outputs must remain well-defined on failure.
- Assertions for internal invariants; explicit error returns for external failures.

C23 usage:
- Prefer: static_assert, nullptr, constexpr objects, designated initializers, fixed-width types where width matters, inline functions over macros.
- Favor compile-time guarantees where possible.

Important build/runtime rule:
- Do not use assert().
- Do not rely on assertions for correctness, validation, or invariants.
- Assume assertions are removed in highly optimized builds.
- Express invariants through API design, explicit validation, error codes, static_assert, types, compile-time constraints, and well-defined control flow.
- If an invariant must be checked at runtime, handle it explicitly with normal control flow and defined error handling.

Testing requirements:
- Write tests as standalone test programs, not only as inline examples.
- Place new tests in `test/`.
- Follow the style and structure of the existing test programs in `test/`.
- Add boundary tests and negative tests where relevant.
- Prefer deterministic tests.
- Include tests for:
  - zero-length / empty cases where valid
  - minimal valid inputs
  - invalid arguments
  - capacity/size limit behavior
  - workspace sizing behavior
  - failure paths
  - correctness of outputs for representative cases
- Keep tests simple, explicit, and easy to run in low-level build environments.

Build:
- Code must compile cleanly under the project's strict warning flags (see build.mk).
- Code must be ASan/UBSan friendly.

Before writing code, first list briefly:
1. core data structures and buffer/array contracts
2. ownership, lifetime, and workspace/scratch model
3. error model
4. unavoidable pointer uses and why

Then provide: proposed API, implementation, performance rationale, tests (including numerical tolerance tests where applicable), and self-review against these rules.
