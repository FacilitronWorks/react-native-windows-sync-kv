# @facilitronworks/react-native-windows-sync-kv

**Synchronous, persistent key/value storage for React Native Windows** on the new architecture (Fabric / Composition) — the storage primitive RNW does not have.

> **Status: pre-release (`0.1.0-pre.1`).**
> The C++ implementation is extracted from a shipping production Windows app (RNW 0.83.2, Windows-on-ARM and x64), where it is the storage engine behind four separate JS packages' Windows shims.
>
> **What is verified here:** see [Verification status](#verification-status). Read it before depending on this.

## Why this exists

React Native Windows ships **no synchronous, JS-accessible persistent store at all**. Not a slow one — none. Every option is either async or absent:

| Package | Windows status |
| --- | --- |
| `@react-native-async-storage/async-storage` | Async by definition — `await` on every read. |
| `react-native-mmkv` v4 | Nitro/native; **no Windows build**. The synchronous API that makes MMKV worth using is unavailable. |
| `react-native-encrypted-storage` | No Windows implementation. |
| `expo-secure-store` | No Windows implementation. |

That gap bites hardest in **offline-first apps**. The MMKV programming model — `const token = storage.getString('token')` returning a *value* at module-load time, not a promise — is what lets a screen render populated on first paint instead of flashing empty and filling in a tick later. Ported to Windows, that code does not merely run slower; it does not run.

This module supplies the missing primitive: a `REACT_SYNC_METHOD`-based store that blocks the JS thread for microseconds and hands back the value.

In the app it was extracted from, this single native module backs Windows shims for **all four** packages above — each one namespaces its keys into the same store.

## How it works

- Hydrates one JSON blob from `ApplicationData.LocalFolder\SyncKV.json` into an in-memory `std::unordered_map` **once**, lazily, on first access (mirroring MMKV's mmap-hydrate-once model), via synchronous `std::ifstream`.
- Serves reads and writes from that map with zero async hops.
- Debounces writes (300 ms quiet period) onto a background thread, then persists by writing a sibling `.tmp` and `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` — so a kill mid-write can only corrupt the temp file, never the real one.
- `clearAll()` flushes **synchronously**, so a logout/purge is durable immediately.

### The lifetime trap this encodes

This is the part worth reading even if you write your own.

React module instances are created and destroyed **per React instance** — every Metro reload tears yours down. The obvious design (map, mutex, condition variable as instance members; a detached worker thread capturing `this`) produces a use-after-free the moment you reload: the orphaned worker keeps reading and writing the freed instance. It presented as random Hermes access violations and fail-fasts, in three distinct crash signatures, and got worse the more data was stored — i.e. it looked like anything except a lifetime bug.

The fix, which this module ships: **all mutable state lives in a deliberately leaked process-lifetime singleton**, and exactly one worker thread exists per process no matter how many times the React instance reloads. A leaked singleton cannot dangle; the OS reclaims it at process death.

### Other constraints encoded here

- **RNW sync methods cannot return `void`** — `ModuleSyncMethodInfo` requires a `TResult`. That is why `set` / `remove` / `clearAll` return `bool`.
- Every method is `noexcept` per the sync-method contract, so **every allocating body is `try`-wrapped**. A `bad_alloc` escaping a `noexcept` function fast-fails the process, and multi-MB values make that reachable.
- Writes are kept synchronous (not `REACT_METHOD`) specifically so a `set()` immediately followed by a `getString()` observes the new value.

## Installation

```sh
npm install @facilitronworks/react-native-windows-sync-kv
# or: yarn add @facilitronworks/react-native-windows-sync-kv
```

Then autolink and rebuild:

```sh
npx react-native autolink-windows
npx react-native run-windows
```

Autolinking derives `#include <winrt/RNWSyncKv.h>` and `packageProviders.Append(winrt::RNWSyncKv::ReactPackageProvider())` from the `<RootNamespace>` in the `.vcxproj`. **A Metro reload will not pick up a new native module — you must rebuild.**

No `WinMain` wiring is required: this module has no host-window dependency.

## Usage

```ts
import {
  getString, set, remove, contains, getAllKeys, clearAll, createStore,
} from '@facilitronworks/react-native-windows-sync-kv';

set('token', 'abc');
getString('token');   // 'abc'  — a value, not a promise
contains('token');    // true
getAllKeys();         // ['token']
remove('token');
clearAll();           // wipes everything, flushes synchronously
```

Namespaced stores, when several logical stores share the one native blob:

```ts
const prefs = createStore('prefs');
prefs.set('darkMode', true);
prefs.getBoolean('darkMode');  // true
prefs.getString('missing');    // undefined  (not '')
prefs.clearAll();              // only clears the 'prefs.' namespace
```

Note the deliberate difference: the flat `getString()` returns `''` for a missing key (the raw native contract), while `createStore(...).getString()` returns `undefined`, matching the react-native-mmkv surface.

### Shimming another package

The typical use is a `.windows.ts` platform extension that re-exports another package's API on top of this store, e.g. for `react-native-mmkv`:

```ts
// mmkvStorage.windows.ts
import { createStore } from '@facilitronworks/react-native-windows-sync-kv';
export const storage = createStore('mmkv');
```

## Traps you will hit

- **Values are strings.** Serialize anything else yourself (`createStore` coerces booleans and numbers via `String()` and parses them back). The on-disk format is a flat JSON object of string→string; a non-string JSON value in a hand-edited file is skipped at hydrate.
- **`getString()` cannot distinguish "missing" from `''`.** Use `contains()` — or `createStore`, which does this for you.
- **This blocks the JS thread.** Storing megabytes per key and reading them in a render path will show up as jank. It is a preferences/session store, not a database.
- **Not encrypted.** `LocalFolder\SyncKV.json` is plain JSON on disk. Shimming `expo-secure-store` or `react-native-encrypted-storage` onto it gives you their *API*, not their *security guarantee*. Do not put secrets here without adding encryption (e.g. DPAPI) yourself.
- **The store is process-wide and unpartitioned.** Two libraries choosing the same key collide. Namespace via `createStore`.
- **Building from source requires a NuGet restore first** (`msbuild -restore`). Without it the CppWinRT targets never load, `.idl` files fall through to classic `midl.exe` instead of `midlrt`, and every `namespace` declaration fails with `MIDL2025`.

## Verification status

**Verified**, on this exact packaged source:

- The packaged form compiles. Autolinked via a `portal:` dependency into a consuming RNW 0.83.2 new-arch app and built clean for **ARM64 Debug**, producing `RNWSyncKv.dll`, `RNWSyncKv.winmd` and `RNWSyncKv.lib`.
- `autolink-windows` resolves the package and emits both the include and the `ReactPackageProvider` registration.

**NOT verified** — do not assume any of these:

- **Runtime behaviour of the DLL built from *this* project.** The source is production-proven inside its original app, but the binary produced by this repo has not been exercised on-device: no read/write/persistence/reload test has been run against it.
- **x64, Win32, and Release** configurations. Only ARM64 Debug was built.
- The `src/index.ts` **JS layer is new to this package** and was not extracted from the production app (which called the native module through its own per-package shims). It typechecks; it has not been run.
- No example app, no automated tests, no CI.
- Behaviour under multiple processes/instances of the same app sharing one `LocalFolder` is undefined and untested — last writer wins, at best.

## Changelog

- **0.1.0-pre.1** — backing store file is now `SyncKV.json` (was `RNWSyncKv.json`), so it adopts an existing `SyncKV.json` store in place; drop-in for consumers migrating off the in-tree module.
- **0.1.0-pre.0** — initial pre-release.

## License

MIT
