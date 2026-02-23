- [x] Add block selection (Ctrl+v when in normal nav mode and forced nav mode)
- [ ] Copying from an ssh is scuffed as fuck since it adds spaces to the copy. Pasting to ssh also does this (It adds spaces where \ or \n should go (nothing just wrapped) this prevents opening curl commands for example)
- [ ] When command line content updates (includes cursor) after a Ctrl+L it always snaps the view such that the top row is the top row after Ctrl+L originally, even if prompt line is in view. It shouldn't do this.
- [ ] If ssh flow fails / abruptly exits / unable to login exit is never called and the ssh popup stays top right
- [ ] Add zz

# Speculative
- [ ] Find a way to sync zsh's cursor to st's not the other way around and replace instances in the code where st is synced to zsh with that. There are many such instances in vimnav.c
