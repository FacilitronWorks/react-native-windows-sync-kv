import { NativeModules, Platform } from 'react-native';

/**
 * The native module, exposed as `NativeModules.RNWSyncKv` once the package
 * provider is registered (autolinking does this — see README: Installation).
 *
 * Every method here is a REACT_SYNC_METHOD: it blocks the JS thread and returns
 * a value directly. That is the entire point of this package.
 */
interface NativeSyncKv {
  getString(key: string): string;
  contains(key: string): boolean;
  /** Returns true on success. RNW sync methods cannot return void. */
  set(key: string, value: string): boolean;
  /** Returns true if the key existed. */
  remove(key: string): boolean;
  getAllKeys(): string[];
  clearAll(): boolean;
}

const Native: NativeSyncKv | undefined = (
  NativeModules as Record<string, NativeSyncKv | undefined>
).RNWSyncKv;

/** True when the native module is present and usable on this platform. */
export function isAvailable(): boolean {
  return Platform.OS === 'windows' && Boolean(Native);
}

function requireNative(): NativeSyncKv {
  if (Platform.OS !== 'windows') {
    throw new Error(
      '@facilitronworks/react-native-windows-sync-kv only runs on Windows. ' +
        "Guard your call with Platform.OS === 'windows', or use it behind a " +
        '`.windows.ts` platform-extension shim.',
    );
  }
  if (!Native) {
    throw new Error(
      'The RNWSyncKv native module was not found. Run `npx react-native ' +
        'autolink-windows` and REBUILD the app — a Metro reload alone will ' +
        'not pick up a new native module.',
    );
  }
  return Native;
}

// ---------------------------------------------------------------------------
// Flat API — operates on the single process-wide store.
// ---------------------------------------------------------------------------

/** Returns `''` for a missing key (the native contract), never null. */
export function getString(key: string): string {
  return requireNative().getString(key);
}

export function contains(key: string): boolean {
  return requireNative().contains(key);
}

export function set(key: string, value: string): boolean {
  return requireNative().set(key, value);
}

export function remove(key: string): boolean {
  return requireNative().remove(key);
}

export function getAllKeys(): string[] {
  return requireNative().getAllKeys();
}

/** Wipes the whole store and flushes to disk SYNCHRONOUSLY (logout/purge). */
export function clearAll(): boolean {
  return requireNative().clearAll();
}

// ---------------------------------------------------------------------------
// Namespaced store — the shape you want when several logical stores share the
// one native blob (e.g. shimming mmkv + async-storage + secure-store at once).
// ---------------------------------------------------------------------------

export interface SyncKvStore {
  getString(key: string): string | undefined;
  getBoolean(key: string): boolean | undefined;
  getNumber(key: string): number | undefined;
  set(key: string, value: string | boolean | number): void;
  delete(key: string): void;
  contains(key: string): boolean;
  getAllKeys(): string[];
  clearAll(): void;
}

/**
 * Returns a store whose keys are transparently prefixed with `${id}.`.
 *
 * `getString` returns `undefined` (not `''`) for a missing key, matching the
 * react-native-mmkv surface rather than the raw native one.
 */
export function createStore(id: string): SyncKvStore {
  const prefix = `${id}.`;
  const full = (key: string) => prefix + key;

  const readRaw = (key: string): string | undefined => {
    const native = requireNative();
    const k = full(key);
    return native.contains(k) ? native.getString(k) : undefined;
  };

  return {
    getString: readRaw,
    getBoolean(key) {
      const raw = readRaw(key);
      return raw === undefined ? undefined : raw === 'true';
    },
    getNumber(key) {
      const raw = readRaw(key);
      if (raw === undefined) return undefined;
      const n = Number(raw);
      return Number.isNaN(n) ? undefined : n;
    },
    set(key, value) {
      requireNative().set(full(key), String(value));
    },
    delete(key) {
      requireNative().remove(full(key));
    },
    contains(key) {
      return requireNative().contains(full(key));
    },
    getAllKeys() {
      return requireNative()
        .getAllKeys()
        .filter((k) => k.startsWith(prefix))
        .map((k) => k.slice(prefix.length));
    },
    clearAll() {
      const native = requireNative();
      for (const k of native.getAllKeys()) {
        if (k.startsWith(prefix)) native.remove(k);
      }
    },
  };
}
