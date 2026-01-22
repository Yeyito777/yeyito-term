- [ ] Add f/F, ;, searching in nav mode
- [ ] Add J/L/M cursor movement (bounded by the prompt line)
- [ ] Add Ctrl+num cursor movement relative to the first line (nvim config-like)
- [ ] Add Ctrl+num keybindings for cursor jumping (bounded by the prompt line)
- [ ] Add gg/G
- [ ] Add zz
- [ ] Add a keybind or way to go to previous command?
- [ ] You cannot enter normal mode or nav mode when you're in a stdout stream because zsh never sends the code. Make sure we can still enter normal mode if we're in a stdout stream and sync back with zsh when it ends or is closed
- [ ] There is a bug that prevents you from going back up in history with either Ctrl+ or with k/j in nav mode, I'm not sure how to replicate

# Next major update
- [ ] Add :, ?, / (Search with regex)
- [ ] Find a way to sync zsh's cursor to st's not the other way around and replace instances in the code where st is synced to zsh with that. There are many such instances in vimnav.c
