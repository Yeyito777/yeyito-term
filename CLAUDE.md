You're in my st fork's source.
This fork coordinates with zsh through .zshrc to be able to do a lot of its operations.
You can check the zsh config required for many of the features in this terminal to work in: README.md.
Make sure to modify the required zsh config in README.md if you make a change that needs a change in it.

## CODE ORGANIZATION
- st.c/st.h - Core terminal functionality
- x.c - X11 windowing
- vimnav.c/vimnav.h - Vim-style navigation mode
- sshind.c/sshind.h - SSH indicator overlay

## TERMINOLOGY & IDIOMS

### Nav mode (vim navigation mode)
- Activated by pressing Escape in the terminal (when zsh is in vi command mode)
- Allows vim-like navigation through terminal history using h/j/k/l, Ctrl+u/d, etc.
- Controlled by `vimnav.mode`: `VIMNAV_INACTIVE`, `VIMNAV_NORMAL`, `VIMNAV_VISUAL`, `VIMNAV_VISUAL_LINE`
- Check with `tisvimnav()` - returns true if nav mode is active

### Prompt space vs history
- **Prompt space**: The current command line area where zsh has cursor control (the prompt + any text being typed)
- **History**: Lines above the prompt containing previous command output - nav mode can scroll through this
- `vimnav_is_prompt_space(y)`: Returns true if screen row y is within the prompt/command area
- `term.scr`: Scroll offset - 0 means viewing bottom (prompt visible), >0 means scrolled up into history

### Snap to prompt
- When pressing editing keys (x, d, c, D, etc.) while scrolled up viewing history, the terminal "snaps back" to the prompt line and passes the key to zsh
- Implemented via `vimnav_snap_to_prompt()` which scrolls down and syncs cursor position

### zsh coordination
- This terminal works closely with zsh's vi mode
- zsh reports cursor position and visual selection state to st via OSC escape sequences
- `vimnav.zsh_cursor`: Cursor position within the command line (reported by zsh)
- `vimnav.zsh_visual`: Whether zsh is in visual selection mode
- Keys in prompt space are passed through to zsh; keys in history are handled by st's nav mode

### Scrollback history buffer
- `term.hist[HISTSIZE]`: Circular buffer storing lines that scrolled off screen
- `term.histi`: Current index in the circular history buffer
- `term.scr`: How many lines we're scrolled back (0 = at bottom)
- `TLINE(y)` macro: Returns the correct line (from history or screen) based on scroll position

### Alt screen
- Alternate screen buffer used by programs like vim, less, htop
- `IS_SET(MODE_ALTSCREEN)`: Check if alt screen is active
- History/scrollback is disabled in alt screen mode

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
