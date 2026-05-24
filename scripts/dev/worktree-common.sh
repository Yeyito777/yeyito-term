#!/usr/bin/env bash
# Shared helpers for st worktree scripts.

WORKTREE_SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
ST_ROOT="$(dirname "$(dirname "$WORKTREE_SCRIPT_DIR")")"

worktree_die() {
  printf "\n  ✗ %s\n\n" "$1" >&2
  exit 1
}

resolve_worktree_dir() {
  local input="${1:-}"
  [[ -n "$input" ]] || worktree_die "Usage: <worktree-name|path>"

  if [[ "$input" == /* ]]; then
    printf '%s\n' "$input"
  elif [[ "$input" == .worktrees/* ]]; then
    printf '%s\n' "$ST_ROOT/$input"
  else
    printf '%s\n' "$ST_ROOT/.worktrees/$input"
  fi
}

registered_worktree_branch() {
  local worktree_dir="$1"
  git -C "$ST_ROOT" worktree list --porcelain | awk -v target="$worktree_dir" '
    $1 == "worktree" { in_target = ($2 == target); next }
    in_target && $1 == "branch" {
      sub(/^refs\/heads\//, "", $2)
      print $2
      exit
    }
  '
}

processes_using_worktree() {
  local worktree_dir="$1"
  local proc pid cwd exe cmd env_line found

  for proc in /proc/[0-9]*; do
    [[ -d "$proc" ]] || continue
    pid="${proc##*/}"
    found=0

    cwd="$(readlink "$proc/cwd" 2>/dev/null || true)"
    if [[ "$cwd" == "$worktree_dir" || "$cwd" == "$worktree_dir"/* ]]; then
      found=1
    fi

    exe="$(readlink "$proc/exe" 2>/dev/null || true)"
    if [[ $found -eq 0 && ( "$exe" == "$worktree_dir" || "$exe" == "$worktree_dir"/* ) ]]; then
      found=1
    fi

    if [[ $found -eq 0 && -r "$proc/environ" ]]; then
      while IFS= read -r env_line; do
        case "$env_line" in
          "PWD=$worktree_dir"|"PWD=$worktree_dir"/*|"HOME=$worktree_dir/.home"|"HOME=$worktree_dir/.home"/*|"ST_TEST_WORKTREE=$worktree_dir")
            found=1
            break
            ;;
        esac
      done < <({ tr '\0' '\n' < "$proc/environ"; } 2>/dev/null || true)
    fi

    if [[ $found -eq 1 ]]; then
      cmd="$(tr '\0' ' ' < "$proc/cmdline" 2>/dev/null || true)"
      printf '    %s %s\n' "$pid" "${cmd:-?}"
    fi
  done
}

prepare_isolated_home() {
  local worktree_dir="$1"
  local home_dir="$worktree_dir/.home"

  mkdir -p "$home_dir/.runtime/st" "$home_dir/.config"
  chmod 700 "$home_dir" "$home_dir/.runtime" "$home_dir/.runtime/st" "$home_dir/.config" 2>/dev/null || true

  # Give an interactive sttest shell a small marker without copying or mutating the
  # user's real shell config.  The terminal itself still gets an isolated HOME so
  # persistence files land in .home/.runtime/st for easy cleanup.
  if [[ ! -e "$home_dir/.sttest-profile" ]]; then
    cat >"$home_dir/.sttest-profile" <<'PROFILE'
# Created by scripts/dev/sttest for an isolated worktree HOME.
export STTEST_ISOLATED_HOME=1
PROFILE
  fi

  printf '%s\n' "$home_dir"
}

cleanup_worktree_runtime() {
  local worktree_dir="$1"
  local wt_name="$(basename "$worktree_dir")"

  rm -rf "$worktree_dir/.home"
  rm -rf "$worktree_dir/.runtime"

  # st's production persistence path is hard-coded as $HOME/.runtime/st/st-<pid>.
  # sttest uses an isolated HOME above, so there should not be per-worktree entries
  # in the user's real runtime dir; these name-based removals are just defensive for
  # future helpers that may create a named runtime directory.
  if [[ -n "${HOME:-}" ]]; then
    rm -rf "$HOME/.runtime/st/$wt_name"
  fi

  git -C "$ST_ROOT" worktree prune >/dev/null 2>&1 || true
}

clean_ignored_build_outputs() {
  local worktree_dir="$1"

  [[ -d "$worktree_dir/.git" || -f "$worktree_dir/.git" ]] || return 0

  if [[ -f "$worktree_dir/Makefile" ]]; then
    make -C "$worktree_dir" clean clean-tests >/dev/null 2>&1 || true
  fi

  # Remove ignored build/runtime artifacts while preserving any non-ignored files
  # the developer may have created intentionally.
  git -C "$worktree_dir" clean -fdX >/dev/null 2>&1 || true
}
