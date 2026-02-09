#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <vector>

#include "../include/ThreadPool.h"

namespace fs = std::filesystem;

namespace MediaProcessor::Tests {

/*
 * LLMEvalTests — Bug Verification Tests for RL Environment
 *
 * These tests verify correct behavior in AudioProcessor::filterChunks().
 *
 * The filterChunks() method processes audio chunks in parallel via a ThreadPool.
 * Each thread returns a bool indicating success or failure. After all threads
 * complete, the results must be aggregated such that if ANY chunk fails, the
 * overall result is false. This requires compound AND assignment (&=) when
 * accumulating results — plain assignment (=) would only retain the last
 * thread's result, silently discarding earlier failures.
 *
 * Affected file: MediaProcessor/src/AudioProcessor.cpp
 * Affected method: AudioProcessor::filterChunks()
 * Affected pattern:
 *     bool allSuccess = true;
 *     for (auto& result : results) {
 *         allSuccess &= result.get();   // MUST be &= not =
 *     }
 */

// ---------------------------------------------------------------------------
// Helper: resolve path to AudioProcessor.cpp from the TEST_MEDIA_DIR macro
// TEST_MEDIA_DIR is defined at compile time as .../tests/TestMedia
// Source lives at ../../src/AudioProcessor.cpp relative to that
// ---------------------------------------------------------------------------
static fs::path getAudioProcessorSourcePath() {
    fs::path testMediaDir(TEST_MEDIA_DIR);
    return testMediaDir.parent_path().parent_path() / "src" / "AudioProcessor.cpp";
}

// ---------------------------------------------------------------------------
// Test 1: Source-level verification
//
// Reads AudioProcessor.cpp and verifies that the thread result aggregation
// loop uses &= (compound AND assignment). This test FAILS when the bug is
// present (plain =) and PASSES when it is fixed (&=).
// ---------------------------------------------------------------------------
TEST(LLMEvalTest, FilterChunks_ThreadResultAggregation_UsesCompoundAndAssignment) {
    fs::path sourcePath = getAudioProcessorSourcePath();

    ASSERT_TRUE(fs::exists(sourcePath))
        << "AudioProcessor.cpp not found at expected path: " << sourcePath << "\n"
        << "Verify that TEST_MEDIA_DIR is set correctly in cmake.";

    std::ifstream file(sourcePath);
    ASSERT_TRUE(file.is_open())
        << "Could not open AudioProcessor.cpp at: " << sourcePath;

    std::string line;
    bool foundAggregationLine = false;
    bool usesCompoundAssignment = false;
    int lineNumber = 0;
    std::string actualLine;

    while (std::getline(file, line)) {
        lineNumber++;

        // Locate the aggregation line: contains both "allSuccess" and "result.get()"
        if (line.find("allSuccess") != std::string::npos &&
            line.find("result.get()") != std::string::npos) {
            foundAggregationLine = true;
            actualLine = line;

            if (line.find("&=") != std::string::npos) {
                usesCompoundAssignment = true;
            }
            break;
        }
    }

    ASSERT_TRUE(foundAggregationLine)
        << "Could not find the thread result aggregation line in AudioProcessor.cpp.\n"
        << "Expected a line containing both 'allSuccess' and 'result.get()' in "
        << "the filterChunks() method.";

    EXPECT_TRUE(usesCompoundAssignment)
        << "\n"
        << "=== BUG DETECTED: Thread Result Aggregation in filterChunks() ===\n"
        << "\n"
        << "  File:     MediaProcessor/src/AudioProcessor.cpp\n"
        << "  Line:     " << lineNumber << "\n"
        << "  Found:   " << actualLine << "\n"
        << "  Expected: allSuccess &= result.get();\n"
        << "\n"
        << "  Using plain '=' instead of '&=' means only the LAST thread's return\n"
        << "  value is kept. If an early chunk fails (e.g. DeepFilterNet crash,\n"
        << "  corrupted audio, disk full) but the final chunk succeeds, filterChunks()\n"
        << "  incorrectly reports success. The merge step then operates on a mix of\n"
        << "  processed and missing/corrupt chunk files, producing silent audio\n"
        << "  corruption or an FFmpeg failure.\n"
        << "\n"
        << "  Fix: change '=' to '&=' so failures are accumulated across all threads.\n";
}

// ---------------------------------------------------------------------------
// Test 2: Behavioral demonstration
//
// Uses the project's own ThreadPool to show that correct aggregation (&=)
// detects an early thread failure. This test always passes on its own —
// its purpose is to demonstrate the expected behavior so the relationship
// between the aggregation operator and the outcome is clear.
// ---------------------------------------------------------------------------
TEST(LLMEvalTest, FilterChunks_ThreadResultAggregation_BehavioralVerification) {
    // Replicate the filterChunks pattern: N threads, each returning a bool
    constexpr int numChunks = 4;
    ThreadPool pool(numChunks);
    std::vector<std::future<bool>> results;

    // Simulate: chunk 0 FAILS, chunks 1-3 succeed
    // In a real run this could be a DeepFilterNet init failure, bad WAV, etc.
    results.emplace_back(pool.enqueue([]() { return false; }));
    results.emplace_back(pool.enqueue([]() { return true; }));
    results.emplace_back(pool.enqueue([]() { return true; }));
    results.emplace_back(pool.enqueue([]() { return true; }));

    // --- Correct aggregation: &= accumulates all results ---
    bool allSuccess_correct = true;
    // We collect values first to test both patterns on the same data
    std::vector<bool> collected;
    for (auto& r : results) {
        collected.push_back(r.get());
    }

    for (bool val : collected) {
        allSuccess_correct &= val;
    }

    EXPECT_FALSE(allSuccess_correct)
        << "Correct aggregation (&=) should report failure when any chunk fails.";

    // --- Buggy aggregation: = overwrites on each iteration ---
    bool allSuccess_buggy = true;
    for (bool val : collected) {
        allSuccess_buggy = val;
    }

    EXPECT_TRUE(allSuccess_buggy)
        << "This demonstrates the bug: with plain '=', allSuccess takes the value\n"
        << "of the last element only. Since the last chunk succeeded, the buggy\n"
        << "aggregation incorrectly reports success despite chunk 0 failing.";

    // The critical assertion: correct and buggy should differ
    EXPECT_NE(allSuccess_correct, allSuccess_buggy)
        << "With mixed results (some fail, some succeed), '&=' and '=' MUST produce\n"
        << "different outcomes. If they don't, the test inputs are wrong.";
}

}  // namespace MediaProcessor::Tests
