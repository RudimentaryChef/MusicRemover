# Bug Tracking — RL Evaluation Environment

## Branch Structure

| Branch | Purpose |
|--------|---------|
| `main` | Documentation and reference (this file) |
| `golden` | Clean, correct codebase — ground truth |
| `baseline` | Codebase with bugs introduced — agent starts here |
| `test` | Test cases that verify whether bugs are fixed |

---

## Bug 1: Thread Result Aggregation — Silent Early Failure Ignored

### Location

- **File**: `MediaProcessor/src/AudioProcessor.cpp`
- **Method**: `AudioProcessor::filterChunks()`
- **Line**: 237

### Description

The `filterChunks()` method processes audio chunks in parallel using a `ThreadPool`. Each thread runs DeepFilterNet on one chunk and returns `true` (success) or `false` (failure). After all threads complete, the method aggregates results to determine if the overall operation succeeded.

The bug changes the aggregation operator from compound AND assignment (`&=`) to plain assignment (`=`):

```cpp
// CORRECT (golden):
bool allSuccess = true;
for (auto& result : results) {
    allSuccess &= result.get();
}

// BUGGY (baseline):
bool allSuccess = true;
for (auto& result : results) {
    allSuccess = result.get();
}
```

With plain `=`, the variable `allSuccess` is overwritten on every iteration. Only the **last** thread's return value determines the outcome. If chunk 0 fails (DeepFilterNet crash, corrupted audio, disk full, etc.) but the final chunk succeeds, `filterChunks()` reports success.

### Impact

- **Severity**: High
- **Symptom**: No error message, no crash. The merge step proceeds with a mix of successfully processed and missing/corrupt chunk files.
- **Result**: Silent audio corruption — garbled or missing sections in the output file, or an FFmpeg merge failure with a misleading error about missing input files.
- **Intermittent**: The bug only manifests when an early chunk fails and a later chunk succeeds. If the last chunk is the one that fails, behavior is coincidentally correct.

### Solution

Change `=` back to `&=` on line 237 of `AudioProcessor.cpp`:

```cpp
allSuccess &= result.get();
```

This ensures that once any thread reports failure, `allSuccess` becomes `false` and stays `false` regardless of subsequent thread results.

### Test

- **File**: `MediaProcessor/tests/LLMEvalTests.cpp` (on `test` branch)
- **Build**: Added to `MediaProcessor/cmake/test.cmake` as `LLMEvalTests` target
- **Tests**:
  - `FilterChunks_ThreadResultAggregation_UsesCompoundAndAssignment` — Reads `AudioProcessor.cpp` source and verifies the aggregation line uses `&=`. Fails on `baseline`, passes on `golden`.
  - `FilterChunks_ThreadResultAggregation_BehavioralVerification` — Uses the project's `ThreadPool` to demonstrate that `&=` and `=` produce different results when an early thread fails and later threads succeed.

---

## Summary of All Changes

### `baseline` branch (1 commit)

| File | Change |
|------|--------|
| `MediaProcessor/src/AudioProcessor.cpp:237` | `allSuccess &= result.get()` → `allSuccess = result.get()` |

### `test` branch (1 commit)

| File | Change |
|------|--------|
| `MediaProcessor/tests/LLMEvalTests.cpp` | New file — GTest test cases for bug verification |
| `MediaProcessor/cmake/test.cmake` | Added `LLMEvalTests` test executable target |

### `golden` branch

No changes. Identical to `main` at initial commit.

### `main` branch

| File | Change |
|------|--------|
| `bugs.md` | This file — bug documentation |