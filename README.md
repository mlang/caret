# caret

`caret` is a cursor-based syntax-aware streaming terminal pager
with keyboard-driven navigation, search, selection and copy.
`tmux` users can think of it as streaming copy-mode.
You can move through output while it arrives, and use the terminal
cursor to set a mark and copy the selection.

## Build

Requirements:

- POSIX-like system
- C++23 compiler
- `ncursesw`

Install:

```sh
cmake -DCMAKE_INSTALL_PREFIX=$HOME/.local .
make install
```

## Usage

With streaming input:

```sh
llm ... | caret
```

With files:

```sh
caret file.txt another-file.txt
```

For file input, caret enables syntax-aware commands automatically when it
recognizes the filename or extension. Use `--language LANG` or `-l LANG` to
override automatic detection for the following files. Input from stdin defaults
to Markdown.

Language options apply to the files that follow them, until the next language
option:

```sh
caret -l c foo.def -l markdown README
```

List available languages:

```sh
caret -L
```

`caret` reads data from standard input or files, but takes keyboard input from
`/dev/tty`, so pipelines remain interactive.

## Keys

Movement:

- `Up` / `Down`, `k` / `j`: move by one wrapped screen row
- `Left` / `Right`, `h` / `l`: move horizontally, crossing row boundaries
- `M-b` / `M-f`: move to the previous/next word, crossing logical lines
- `C-M-b` / `C-M-f`: move to the previous/next matching bracket
- `%`: move to the matching bracket, or the next bracket on the current line
- `C-a` / `C-e`, `Home` / `end`, `0` / `$`: beginning/end of current logical line
- `^` / `M-m`: move to the first non-whitespace character of the current logical line
- `PageUp` / `PageDown`, `Ctrl-B` / `Ctrl-F`: move by nearly one screen
- `zt` / `zz` / `zb`: place the cursor row at the top/middle/bottom of the viewport
- `Ctrl-L`: recenter the viewport on the cursor (equivalent to `zz`)
- `M-<` / `M->`, `g g` / `G`: start/end of document
- `:<N>`: move to the start of logical line `<N>` (1-based, clamped to the document)
- `:n` / `:p`: switch to the next/previous file
- `Space`: page down

Selection and copy:

- `C-Space`: set mark at cursor
- `x`: exchange cursor and mark
- `M-w`: copy region between mark and cursor
- `C-k`: copy from cursor to end of current logical line

Syntax selection:

- `Enter`: select syntax node at cursor
- `u` / `p`: previous/next sibling syntax range
- `i` / `o`: shrink/expand syntax selection

Search:

- `C-s` / `C-r`: incremental literal search forward/backward
- `/` / `?`: forward/backward regex search
- `*` / `#`: search forward/backward for the keyword at or after the cursor
- `n` / `N`: repeat last search in the same/opposite direction
- `f` / `F` then character: next/previous matching character in current line

Quit:

- `q`, `Q`, `C-c`, `C-d`

## Prompts

The `/`, `?`, and `:` prompts share these editing keys:

- `Left` / `Right`, `Ctrl-B` / `Ctrl-F`: move the prompt cursor
- `Home` / `End`, `Ctrl-A` / `Ctrl-E`: move to the start/end of the prompt
- `Backspace`: delete before the cursor, or close an empty prompt
- `Delete`: delete the character under the cursor
- `C-u`: delete from the cursor to the start of the prompt
- `C-k`: delete from the cursor to the end of the prompt
- `Esc`, `Ctrl-G`: cancel

In the `:` prompt, a number moves to the start of that logical line. Line
numbers are 1-based. `:n` and `:p` switch between files without wrapping.
`Up` / `Down` browse its session-local command history, which
includes every non-empty submitted command and is separate from search history.

## Regex search

Searches are line-local and use ECMAScript regex syntax.

In the search prompt:

- `Enter`: submit search
- `Up` / `Down`: browse session-local search history

Non-empty submitted searches and keyword searches are saved in history. `/`,
`?`, `*`, and `#` share the same history.

When a match is found, the cursor moves to the start of the match and the mark
is set to the end, highlighting the match. `M-w` then copies the match.

## Incremental search

Incremental searches are line-local literal searches. The search string is shown
in the status line while matches are found as you type. `Ctrl-S` repeats forward
and `Ctrl-R` repeats backward. `Backspace` edits the search string.

`C-g` cancels incremental search and restores the cursor and mark. Movement
and other non-text command keys end incremental search at the current match,
then run normally.

## Copy output

The last region copied with `M-w` is printed to standard output after `caret`
leaves the alternate screen.

Inside tmux, `M-w` sends the region to:

```sh
tmux load-buffer -
```

If `brltty-clip` is installed, the region is also sent to it.

## Notes

- Regular files are loaded directly as UTF-8. Streaming input from stdin and
  FIFOs is decoded using the current locale’s multibyte encoding; all active
  streams are polled concurrently.
- The program links against `ncursesw`.
- ANSI CSI and OSC escape sequences in the input stream are ignored.
