# Contributing to Geranium

## Code style

Use clang-format 18 with the checked-in `.clang-format`, copied from Linux
v6.18. Apply it to production C sources, headers, and C test fixtures. Use
eight-column tabs, an 80-column target, and function braces on a separate
line.

Run the formatter and check its output before submitting changes:

```sh
make fmt
make check-format
```

Set `CLANG_FORMAT=clang-format-18` on either command if the executable has a
versioned name. Keep formatting-only changes separate from behavior changes.

### Vertical whitespace

Use vertical whitespace sparingly. Unnecessary blank lines make it harder
to see the overall structure of the code.

- Always insert one blank line after the declaration section.
- Put one blank line before and after each natural block of code. A natural
  block is a group of statements that perform one logical step, not every
  brace-delimited block. Do not add blank lines immediately inside opening
  or closing braces just to surround a block.
- Separate logically distinct phases of a function with one blank line.
- Keep closely related statements together. An assignment and its following
  error check belong together, with no intervening blank line.
- Never insert a blank line merely because two statements have different
  syntax. A call, an assignment, an `if`, or a `return` does not by itself
  start a new logical phase.
- Use a single blank line at a boundary, not several consecutive blank lines.

For example, querying metadata and checking that the path is a directory
are distinct phases. Each operation stays with its error handling:

```c
#include <errno.h>
#include <sys/stat.h>

int check_directory(const char *path)
{
	struct stat st;
	int result;

	result = stat(path, &st);
	if (result < 0)
		return -1;

	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	return 0;
}
```

The blank line after the declarations separates them from the function
body. The next blank line separates the metadata query from validation.
There is no blank line between the assignment and its error check, or
between related statements solely because their syntax differs.

Clang-format handles mechanical formatting. Review logical grouping and
blank-line placement manually.

## Python test sources

Use Python 3.11 or newer and [uv](https://docs.astral.sh/uv/) for development.
`pyproject.toml` and `uv.lock` pin Ruff and ty. The Make targets use
`uv run --locked`, which creates the development environment automatically
and rejects an out-of-date lockfile.

After configuring the C build, run:

```sh
make fmt
make check-format
make lint
```

`fmt` and `check-format` cover C with clang-format 18 and Python with Ruff.
`lint` checks Python with Ruff and ty, treating type-checker warnings as
errors. Keep Python helpers in `.py` files, not shell heredocs, so both tools
check them. Fix diagnostics rather than adding blanket suppressions.

For Python-only work before configuring the C toolchain, run
`make -f tools.mk lint`. CI uses the same target. Build and runtime tests
still use the system `python3` directly and do not require uv, Ruff, or ty.
Do not add these development tools as runtime dependencies.
