- [ ] There are some prompt line issues and inconsistensies when it comes to snapping / going to the prompt line, identify them and fix them one by one.
- [ ] Add a keybind or way to go to previous command
- [ ] You cannot enter normal mode or nav mode when you're in a stdout stream because zsh never sends the code. Make sure we can still enter normal mode if we're in a stdout stream and sync back with zsh when it ends or is closed
- [ ] Copying from an ssh is scuffed as fuck since it adds spaces to the copy. Pasting to ssh also does this (It adds spaces where \ or \n should go (nothing just wrapped) this prevents opening curl commands for example)
- [ ] Add Ctrl+num cursor movement relative to the first line (nvim config-like)
- [ ] Add zz

# Next major update
- [ ] Add :, ?, / (Search with regex)
- [ ] Find a way to sync zsh's cursor to st's not the other way around and replace instances in the code where st is synced to zsh with that. There are many such instances in vimnav.c
