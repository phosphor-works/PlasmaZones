<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# ADR-0001: Luau as the shell scripting and configuration language

This page records the decision to use Luau as the scripting and configuration language of the shell, and the
reasons for it. It is the first architecture decision record (ADR) of PlasmaZones. Developers read it before they
change a scripting surface.

| | |
|---|---|
| **Deciders** | Nathan (fuddlesworth) |
| **Scope** | Shell/compositor scripting surface; first increment = scripted autotiling |
| **Supersedes** | — |
| **Spike** | `spike/luau-autotile-embed` (throwaway, `spikes/luau-autotile-embed/`) |

## Context

PlasmaZones grows from a window-tiling tool into a full **shell and compositor**, and a Qt/QML shell exists. A shell
needs a scripting and configuration surface for users that is much larger than tiling. It includes keybindings,
rules, widgets, IPC and automation, and compositor plugins. The language for that surface is a basic decision. After
those surfaces exist, a change of language costs approximately 10 times more.

Before this decision, scripted autotiling ran on the **QJSEngine** (JavaScript) of Qt:

- Approximately 3,285 LOC of C++ glue in `libs/phosphor-tiles`: a loader, a **225-line sandbox**
  (`scriptedalgorithmsandbox.cpp`), an interrupt watchdog, builtins injection and split-tree handling.
- **25 algorithms in approximately 2,157 LOC of JS** in `data/algorithms/`.
- QJSEngine is part of Qt6. It adds no dependencies and marshals `QJSValue` to Qt natively. But it has **no true
  sandbox** ("JavaScript can do anything the C++ can") and **no memory limit for each script**. The second gap is
  structural.

**The ecosystem uses Lua.** Lua is the configuration and scripting language of the Wayland tiling niche: AwesomeWM,
Neovim, WezTerm and WirePlumber use it. **Hyprland moved its full configuration to Lua**, and **Noctalia v5
(Quickshell) moves to Lua**. The target users already use Lua for shell scripting. JavaScript is the language of QML
and the web, not of window-manager configuration.

## Decision

Use **Luau** ([luau.org](https://luau.org), MIT) as the **scripting and configuration language of the full shell**.

1. Luau is the language for **new** shell scripting surfaces: configuration, keybindings, rules, widgets and
   plugins.
2. The scripted autotiling engine moves from QJSEngine to Luau as the **first increment**. Tiling follows the
   decision for the shell. It does not drive that decision.
3. The target architecture is a **C++ compositor core, QML for the first-party UI, and Luau for user configuration,
   scripts and plugins.** The **QML↔Luau boundary is an explicit interface with an owner**, designed first.
4. **Library split:** a generic library, **`libs/phosphor-scripting`** (LGPL-2.1), owns the Luau host. The host has
   the engine, sandbox, watchdog, compile and load, and the marshalling core, and it knows nothing about tiling.
   Thus future shell surfaces can use it again. `libs/phosphor-tiles` depends on it and implements the tiling
   binding: `LuauTileAlgorithm`, the `pluau` stdlib and `.d.luau`, and the marshalling of params, state and tree.
5. **Luau is built from a pinned source archive by default**, with a system Luau as an option. The reason is that
   Luau is packaged only for Arch and Nix (not Debian, Fedora or openSUSE) and has no stable ABI.

### Why Luau

| Need | Luau | Stock Lua 5.x / LuaJIT | QJSEngine (status quo) |
|---|---|---|---|
| Sandbox for untrusted plugins | ✅ `luaL_sandbox` (readonly globals/stdlib) | ❌ none built-in | ❌ "no sandboxing" |
| Per-script memory cap | ✅ custom allocator | ✅ custom allocator | ❌ structural gap |
| Runaway-script interrupt | ✅ interrupt callback | ⚠️ debug hooks | ✅ `setInterrupted` |
| Author-time typing | ✅ gradual types + `luau-analyze` | ❌ | ❌ |
| Ecosystem fit (Wayland WMs) | ✅ where it converged | ⚠️ partial | ❌ |
| License / deps | MIT, zero deps, CMake | MIT | bundled w/ Qt |

## Evidence from the embedding spike

A throwaway standalone binary (`spikes/luau-autotile-embed/`, pinned Luau 0.723, Qt6::Core) proved the full path.
**All checks passed:**

- **Build:** FetchContent Luau and the Qt6 link took **30s** with one `cmake` command. The static binary was
  **2.8M**. When the algorithms are precompiled to bytecode, the runtime ships only `Luau.VM` (**1.4M**), without
  the compiler and the AST.
- **The sandbox costs no code:** `luaL_sandbox` with readonly globals **replaces all of the 225-line
  `scriptedalgorithmsandbox.cpp` and the freeze machinery**. An escape probe showed these results: `io` is absent,
  `os` is restricted (no `execute`/`exit`/`getenv`), there is no `loadstring`/`load`, and the stdlib is read-only.
  **Note:** the sandbox does *not* remove `getfenv`/`setfenv`. But it makes their environment read-only, so a
  `getfenv(0).x = 1` write **fails**. The guarantee holds for a different reason than removal.
- **The watchdog is safer:** an atomic flag, an interrupt callback and a `luaL_error` abort stop
  `while true do end` in approximately 150ms, and the host recovers. The watchdog **never touches the `lua_State`**.
  Thus it does not need the use-after-free lock discipline of the QJSEngine watchdog.
- **The marshalling cost is 57 LOC** (35 in, 22 out). This is the new code that replaces the free
  `QJSValue::setProperty`.
- **Ergonomics:** `master-stack` is **approximately 14 LOC** against a typed `pluau` Rect API. The JS version used
  an injected helper of approximately 100 lines.

## Consequences

### Positive

- The change deletes approximately 400 LOC of the **most dangerous C++ with the most maintenance**: the sandbox,
  the freeze for each helper, and the IIFE workaround for the `eval`/`Function` escape. Each of these lines is
  critical for security and decays easily.
- PlasmaZones gets a **real sandbox, a memory limit and typing at authoring time**. This is the correct base for an
  ecosystem of untrusted plugins.
- The shell has **one scripting language**, as users and the ecosystem expect.
- The watchdog is simpler and safer.

### Negative

- A **new third-party dependency**. Before, there was none, because QJSEngine is in Qt. It adds build complexity,
  version pins and the compile-to-bytecode step.
- **Approximately 57 LOC of manual C↔Lua marshalling** replace the free `QJSValue::setProperty`, with similar code
  for hooks and metadata.
- A one-time **rewrite**: approximately 25 algorithms (approximately 2,157 LOC of JS to Luau), and changes to the
  C++ glue.
- A **permanent QML↔Luau bridge** to design and maintain. QML uses JS natively, so Luau is a *second* language in
  the stack. Hyprland does not have this cost.
- The C API of Luau has **no formal stability guarantee**. Pin a version, and expect some additive or breaking
  changes.

**The net change of C++ glue is approximately −350 LOC**: approximately −400 of security code and approximately
+150 of marshalling and setup. The watchdog size stays approximately the same, but the watchdog is safer.

## Order of the work

The first proposal was to start with "new shell surfaces first". The decision changed to start with **tiling as the
first consumer of `phosphor-scripting`**, for these reasons:

- Tiling has a contained contract and a test suite.
- Its path is pure **C++↔Luau with no QML bridge**.

Thus tiling validates the embedding core without the more difficult QML↔Luau UI boundary. The QML↔Luau bridge is
separate work for the shell UI surfaces. The tiling increment kept the `TilingAlgorithm` interface stable and ported
the 25 algorithms under a golden parity test.

## Alternatives

| Alternative | Advantages | Why rejected |
| --- | --- | --- |
| Stay on QJSEngine (JS) | No new dependencies. The same language as the QML UI. | No sandbox, no memory limit, JS is not the configuration language of the ecosystem, and the security code stays. It is the wrong base for a plugin ecosystem. |
| Stock Lua 5.x / LuaJIT | Less embedding work. The full stdlib. | No built-in sandbox for untrusted code, no gradual typing, and not where the ecosystem (Hyprland in particular) went. The sandbox and the typing of Luau are the reason for the decision. |
| All-Lua UI (as AwesomeWM, without QML) | No bridge between two languages. | It discards the QML work that exists and needs a much larger rewrite. |

## Current state

The tiling increment is complete:

- `phosphor/libs/phosphor-scripting`, now `libs/phosphor-scripting` in phosphor-libs, is the LGPL Luau host. It has
  the engine, `luaL_sandbox`, the interrupt watchdog, a **heap limit** for each script, compile and load, and
  QVariant marshalling. It came from the `LuauHost` of the spike. The heap limit is a custom capped `lua_Alloc`, enforced after the sandbox is on.
- `phosphor/libs/phosphor-tiles`, now `libs/phosphor-tiles` in phosphor-libs, has `LuauTileAlgorithm` and the `pluau`
  stdlib.
- All 25 algorithms are ported under a golden-snapshot parity test, and the loader reads `.luau` files.
- The QJSEngine path is deleted.
- A CI **`luau-analyze` gate** checks the bundled algorithms and the `pluau` stdlib.
- An end-user [Luau algorithm authoring guide](https://phosphor-works.org/guides/tiling/) is available.

The two safety nets from the spike are both built. The interrupt watchdog limits CPU *time*, and the capped allocator
limits the *heap*. The heap limit is 64 MiB by default, and each engine can set its own. Enforcement starts at
`sandbox()`, so trusted init and preludes do not get a false out-of-memory stop. A runaway allocation gives a Luau
out-of-memory error that the host can catch. It does not crash the host.

At first, Luau was a committed source tarball (`phosphor/extern/luau-0.737.tar.gz`), extracted at configure time with
`file(ARCHIVE_EXTRACT)`, and `-DPLASMAZONES_SYSTEM_LUAU=ON` linked a system Luau. Now `cmake/PhosphorDependencies.cmake`
of phosphor-libs pins the Luau source archive (version, URL and SHA-256). An offline packaging build reads it from
the `PHOSPHOR_DEPS_ARCHIVES` directory, and other builds download it with CPM. `-DPHOSPHOR_SYSTEM_LUAU=ON` links a
system Luau.
