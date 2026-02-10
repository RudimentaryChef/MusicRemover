#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
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
 * overall result is false.
 *
 * The bug: using plain assignment (allSuccess = result.get()) only retains
 * the last thread's result, silently discarding earlier failures.
 *
 * Valid fixes include:
 *   allSuccess &= result.get();
 *   allSuccess = allSuccess && result.get();
 *   allSuccess = allSuccess & result.get();
 *   if (!result.get()) allSuccess = false;
 *   ... or any other pattern that accumulates failures.
 *
 * Affected file: MediaProcessor/src/AudioProcessor.cpp
 * Affected method: AudioProcessor::filterChunks()
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
// Helper: strip leading/trailing whitespace from a string
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// Test 1: Source-level verification (accepts any valid accumulation fix)
//
// Reads AudioProcessor.cpp and verifies that the thread result aggregation
// loop does NOT use plain assignment (the bug). Any correct accumulation
// form is accepted: &=, &&, conditional, etc.
// ---------------------------------------------------------------------------
TEST(LLMEvalTest, FilterChunks_ThreadResultAggregation_UsesCorrectAccumulation) {
    fs::path sourcePath = getAudioProcessorSourcePath();

    ASSERT_TRUE(fs::exists(sourcePath))
        << "AudioProcessor.cpp not found at expected path: " << sourcePath << "\n"
        << "Verify that TEST_MEDIA_DIR is set correctly in cmake.";

    std::ifstream file(sourcePath);
    ASSERT_TRUE(file.is_open())
        << "Could not open AudioProcessor.cpp at: " << sourcePath;

    // Read the entire file to analyze the aggregation pattern
    std::string fileContent((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    file.close();

    // Find the filterChunks method and its result aggregation loop.
    // We look for the loop over results that assigns to allSuccess.
    // The buggy pattern is: allSuccess = result.get()  (plain assignment)
    // This only keeps the LAST result, discarding earlier failures.

    std::istringstream stream(fileContent);
    std::string line;
    bool foundAggregationLine = false;
    bool usesCorrectAccumulation = false;
    int lineNumber = 0;
    std::string actualLine;

    // Track if we're inside the for loop over results
    bool inResultsLoop = false;
    bool foundFailureCheck = false;

    while (std::getline(stream, line)) {
        lineNumber++;
        std::string trimmed = trim(line);

        // Detect the for loop over results
        if (trimmed.find("for") != std::string::npos &&
            trimmed.find("result") != std::string::npos) {
            inResultsLoop = true;
            continue;
        }

        // Inside the results loop, look for the aggregation pattern
        if (inResultsLoop) {
            // Check for allSuccess and result.get() on the same line
            if (trimmed.find("allSuccess") != std::string::npos &&
                trimmed.find("result.get()") != std::string::npos) {
                foundAggregationLine = true;
                actualLine = trimmed;

                // Accept compound assignment: &=
                if (trimmed.find("&=") != std::string::npos) {
                    usesCorrectAccumulation = true;
                }
                // Accept: allSuccess = allSuccess && result.get()
                // or:     allSuccess = allSuccess & result.get()
                // i.e., allSuccess appears on the RHS of the assignment
                else {
                    auto eqPos = trimmed.find('=');
                    if (eqPos != std::string::npos) {
                        std::string rhs = trimmed.substr(eqPos + 1);
                        if (rhs.find("allSuccess") != std::string::npos) {
                            usesCorrectAccumulation = true;
                        }
                    }
                }
                break;
            }

            // Check for conditional patterns:
            //   if (!result.get()) allSuccess = false;
            //   if (result.get() == false) allSuccess = false;
            if (trimmed.find("result.get()") != std::string::npos &&
                trimmed.find("allSuccess") != std::string::npos &&
                (trimmed.find("if") != std::string::npos ||
                 trimmed.find("?") != std::string::npos)) {
                foundAggregationLine = true;
                actualLine = trimmed;
                usesCorrectAccumulation = true;
                break;
            }

            // Check for two-line conditional: if (!result.get()) on one line
            if (trimmed.find("if") != std::string::npos &&
                trimmed.find("result.get()") != std::string::npos) {
                foundFailureCheck = true;
                continue;
            }
            // ... followed by allSuccess = false on next line
            if (foundFailureCheck &&
                trimmed.find("allSuccess") != std::string::npos &&
                trimmed.find("false") != std::string::npos) {
                foundAggregationLine = true;
                actualLine = "(conditional pattern across multiple lines)";
                usesCorrectAccumulation = true;
                break;
            }
            if (foundFailureCheck && !trimmed.empty() && trimmed != "{") {
                foundFailureCheck = false;
            }

            // Exit loop detection on closing brace (simple heuristic)
            if (trimmed == "}" && !foundAggregationLine) {
                inResultsLoop = false;
            }
        }
    }

    ASSERT_TRUE(foundAggregationLine)
        << "Could not find the thread result aggregation line in AudioProcessor.cpp.\n"
        << "Expected a line in the filterChunks() results loop that assigns to "
        << "'allSuccess' using 'result.get()'.";

    EXPECT_TRUE(usesCorrectAccumulation)
        << "\n"
        << "=== BUG DETECTED: Thread Result Aggregation in filterChunks() ===\n"
        << "\n"
        << "  Found:   " << actualLine << "\n"
        << "\n"
        << "  The aggregation must accumulate results across ALL threads.\n"
        << "  Plain assignment (allSuccess = result.get()) only keeps the LAST\n"
        << "  thread's result, silently discarding earlier failures.\n"
        << "\n"
        << "  Valid fixes include:\n"
        << "    allSuccess &= result.get();\n"
        << "    allSuccess = allSuccess && result.get();\n"
        << "    if (!result.get()) allSuccess = false;\n";
}

// ---------------------------------------------------------------------------
// Test 2: Behavioral verification
//
// Uses the project's own ThreadPool to verify that correct result
// aggregation detects an early thread failure. Simulates the exact
// pattern from filterChunks() with mixed success/failure results.
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

    // Collect results (same as filterChunks loop)
    std::vector<bool> collected;
    for (auto& r : results) {
        collected.push_back(r.get());
    }

    // --- Correct aggregation: accumulates all results ---
    bool allSuccess_correct = true;
    for (bool val : collected) {
        allSuccess_correct &= val;
    }

    EXPECT_FALSE(allSuccess_correct)
        << "Correct aggregation should report failure when any chunk fails.";

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
        << "With mixed results (some fail, some succeed), correct and buggy\n"
        << "aggregation MUST produce different outcomes.";
}

}  // namespace MediaProcessor::Tests
