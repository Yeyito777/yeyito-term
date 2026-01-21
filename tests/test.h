/* See LICENSE for license details. */
/* Minimal test framework for st */

#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Current test name for error reporting */
static const char *current_test = NULL;

/* Colors for output */
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define RESET   "\033[0m"

/* Test declaration macro */
#define TEST(name) static void test_##name(void)

/* Run a test */
#define RUN_TEST(name) do { \
	current_test = #name; \
	tests_run++; \
	printf("  Running %s... ", #name); \
	fflush(stdout); \
	test_##name(); \
	printf(GREEN "PASS" RESET "\n"); \
	tests_passed++; \
} while (0)

/* Assertions */
#define ASSERT(cond) do { \
	if (!(cond)) { \
		printf(RED "FAIL" RESET "\n"); \
		printf("    %s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #cond); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define ASSERT_EQ(expected, actual) do { \
	if ((expected) != (actual)) { \
		printf(RED "FAIL" RESET "\n"); \
		printf("    %s:%d: Expected %d, got %d\n", __FILE__, __LINE__, \
		       (int)(expected), (int)(actual)); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define ASSERT_STR_EQ(expected, actual) do { \
	if (strcmp((expected), (actual)) != 0) { \
		printf(RED "FAIL" RESET "\n"); \
		printf("    %s:%d: Expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, \
		       (expected), (actual)); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define ASSERT_NULL(ptr) do { \
	if ((ptr) != NULL) { \
		printf(RED "FAIL" RESET "\n"); \
		printf("    %s:%d: Expected NULL\n", __FILE__, __LINE__); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define ASSERT_NOT_NULL(ptr) do { \
	if ((ptr) == NULL) { \
		printf(RED "FAIL" RESET "\n"); \
		printf("    %s:%d: Expected non-NULL\n", __FILE__, __LINE__); \
		tests_failed++; \
		return; \
	} \
} while (0)

/* Test suite declaration */
#define TEST_SUITE(name) static void suite_##name(void)

/* Run a test suite */
#define RUN_SUITE(name) do { \
	printf("\n" YELLOW "Suite: %s" RESET "\n", #name); \
	suite_##name(); \
} while (0)

/* Print test summary and return exit code */
static inline int test_summary(void) {
	printf("\n----------------------------------------\n");
	printf("Tests: %d | ", tests_run);
	if (tests_passed > 0)
		printf(GREEN "Passed: %d" RESET " | ", tests_passed);
	if (tests_failed > 0)
		printf(RED "Failed: %d" RESET, tests_failed);
	else
		printf("Failed: 0");
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}

#endif /* TEST_H */
