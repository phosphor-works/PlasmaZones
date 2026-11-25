<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Contributing to PlasmaZones

This page tells contributors how to build PlasmaZones, which code style and license headers to use, how to test a
change, how to send it, and how to update the translations.

## Build for development

Clone the repository and build it locally first. The README has the build steps. This is the short version:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build . -j$(nproc)
```

Debug builds are slower. They give better error messages, and failed asserts stop the program.

## Code style

PlasmaZones follows the KDE coding conventions. The repository root has a `.clang-format` file. Run it on your
changes before you send them:

```bash
clang-format -i plasmazones/src/path/to/your/file.cpp
```

| Item | Rule |
| --- | --- |
| Indentation | 4 spaces, no tabs |
| Braces | On their own line for functions and classes. On the same line for control flow. |
| Pointer asterisk | With the type: `int* foo`, not `int *foo` |
| Line length | 120 characters maximum |

If you are not sure about a rule, look at the code near your change and do the same.

## License headers

Every source file starts with an SPDX header in this format:

```cpp
// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later
```

Phosphor Works Team is the only copyright holder of the project's own work. Do not add your name to the copyright
line. A file that you port from another project also keeps the copyright line of that project.

## Send a change

1. Fork the repository.
2. Create a branch for your work.
3. Make your changes.
4. Test your changes. At a minimum, make sure that the project builds and runs.
5. Open a pull request.

Keep each commit to one logical change. Small commits are easier to review than one large commit with unrelated
changes.

Write a clear commit message. The first line is a short summary in the Conventional Commits format. The commit hook
checks this format. Then add a blank line and more detail if necessary. A message such as "fix bug" does not tell
the reviewers anything.

## Tests

The test suites are in `plasmazones/tests/`. The suites of the libraries also build and run with PlasmaZones. To run
all of them:

```bash
cd build
ctest --output-on-failure --no-tests=error
```

If you add a function, add tests for it. If you fix a bug, a test that finds the bug is welcome.

## Translations

PlasmaZones uses Qt Linguist (`.ts`/`.qm`) for all translations. C++ uses `PhosphorI18n::tr()`. QML uses `i18n()`
through `PhosphorLocalizedContext`.

- **Extract strings** after you change text that users see:
  `cmake --build build --target update-ts`
  This runs `lupdate` and updates `plasmazones/translations/*.ts`. Commit the results.
- **Add or update a translation**: see [`plasmazones/translations/README.md`](plasmazones/translations/README.md).
- **Build and install**: the build compiles the `.ts` files to `.qm` and installs them.

## Questions

If you are stuck or want to discuss a change, open an issue or ask on the [Discord](https://discord.gg/9CQzAptdJ5).
There is no mailing list.
