You're in my st fork's source.
This fork coordinates with zsh through .zshrc to be able to do a lot of its operations.
You can check the zsh config required for many of the features in this terminal to work in: README.md.
Make sure to modify the required zsh config in README.md if you make a change that needs a change in it.

## CODE ORGANIZATION 
- st.c/st.h - Core terminal functionality
- x.c - X11 windowing
- vimnav.c/vimnav.h - Vim-style navigation mode
- sshind.c/sshind.h - SSH indicator overlay

## QA
Whenever you finish an addition to the codebase, run all the tests with `make test`

### Bug fixes and regressions
When fixing bugs, regressions, or anything described as "broken", "not working", "crashed", etc:
1. Fix the issue
2. **Automatically** add a test that verifies the fix (do not ask, just do it)
3. Run all tests with `make test`
4. Done when all tests pass

## TESTING
- Test files are in `tests/` directory
- Run tests: `make test`
- Clean test artifacts: `make clean-tests`
- Test framework: minimal custom framework in `tests/test.h`
- Mocks for terminal state: `tests/mocks.c` and `tests/mocks.h`

### Adding new tests
1. Add test functions in existing test file or create new `tests/test_*.c`
2. Use `TEST(name)` macro to define tests
3. Use `RUN_TEST(name)` in the suite to run them
4. Assert with: `ASSERT(cond)`, `ASSERT_EQ(expected, actual)`, `ASSERT_STR_EQ(expected, actual)`
5. Update Makefile if adding new test files 
