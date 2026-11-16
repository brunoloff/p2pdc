# Fresh Android WebRTC dependencies

An Android arm64 WebRTC stack was rebuilt from freshly fetched GLib 2.88.3,
libnice 0.1.24, libdatachannel 0.23.2, and their support sources against the
fresh OpenDHT dependency prefix. The same build also used a freshly downloaded,
checksum-verified libsodium 1.0.22. The resulting public C++ echo exchanged
messages between an Android phone and Firefox on different networks, both
directly and via automatic TURN fallback with direct ICE checks blocked. The
no-TURN control failed as expected.

For GLib, fetch the pinned source into a new directory. Run these commands from
the library root after setting the prefixes and NDK variable described in
[Android testing](ANDROID.md):

```sh
git clone --depth 1 --branch 2.88.3 https://github.com/GNOME/glib.git .deps/glib-android-src
test "$(git -C .deps/glib-android-src rev-parse HEAD)" = \
  43bc79ea8803e33c5eb368085e2d5906f9f98079
```

Use the GLib Meson setup and targeted build commands in the Android notes, but
omit `--wrap-mode=nodownload` on the first setup. Meson fetched and verified
PCRE2, libffi, and proxy-libintl from their wrap definitions, and fetched the
pinned GVDB Git submodule. The libnice and patched libdatachannel commands in
the Android notes were then run using the fresh GLib and OpenDHT prefixes.

This check used a locally cloned copy of the pinned Jami recipe repository for
the OpenDHT support libraries, although its source archives were fetched fresh.
The documented complete procedure has not yet been run end-to-end from a fresh
checkout of this library.
