# NRZLoader app

Patcher and launcher. Takes the copy of Minecraft already on the device, adds
the mod loader to it, signs the result and installs it.

Nothing from the game is redistributed. The app ships only its own code: the
loader library, a bootstrap class, and a signing key. The game package comes
from the device.

## How the loader gets in

The earlier desktop route rewrote the game library's ELF dependencies, which
needs patchelf and is not available on a phone. The app takes a simpler route:

1. a tiny `classesN.dex` is added with a `ContentProvider` that calls
   `System.loadLibrary("nrzloader")`;
2. the manifest gains a `<provider>` entry pointing at it.

Providers are created before any activity and alongside whatever `Application`
the game declares, so the library loads early without displacing anything the
game relies on. Android loads every `classes*.dex` in a package, so the added
one needs no other registration.

The manifest also gains all-files access and a new application label.

## Layout

```
app/
├── libs/apksig.jar          Google's signing library, used as is
├── src/main/assets/         bootstrap dex, signing key, loader library
├── src/main/java/.../patch/ the patch engine: pure Java, no Android calls
└── src/test/java/.../patch/ tests, runnable on a desktop JVM
```

The patch engine deliberately avoids Android APIs so it can be tested without
a device.

## Tests

```bash
tests/run_app_tests.sh
```

Manifest edits are checked against a manifest aapt2 produced, and the signed
output is verified with Google's own `apksigner`.
