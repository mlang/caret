# Third-party code

This directory contains vendored third-party code.

## bash_scanner.c

The external scanner from [tree-sitter-bash](https://github.com/tree-sitter/tree-sitter-bash).

This file is vendored for compatibility with nvim-treesitter, which builds
parsers without npm/node_modules available.

**License:** MIT (see LICENSE.tree-sitter-bash)

**To update after `npm install`:**

```sh
make update-bash-scanner
```

**To check if up to date:**

```sh
make check-bash-scanner
```
