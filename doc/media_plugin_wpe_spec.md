# `media_plugin_wpe` — WebKit media plugin spec (BSD/Linux MOAP without CEF)

## Why

CEF/Chromium has no practical FreeBSD build, so Megapahit-style BSD viewers ship
without web media-on-prim. Because MOAP is core to our use, we replace the CEF
backend (`media_plugin_cef` + Dullahan) with a **WPE WebKit** backend that is
BSD- and Linux-native, without touching the viewer core.

Key architectural fact: the browser is a **media plugin** running in the
`SLPlugin` child process, behind the `LLPluginClassMedia` message protocol —
the *same* protocol `media_plugin_libvlc` uses. So this is a self-contained new
plugin (`indra/media_plugins/wpe/`), not a viewer-core change. The viewer never
knows which engine drew the pixels.

## Engine choice: WPE WebKit (not WebKitGTK, not Gecko)

- **WPE WebKit** — "Web Platform for Embedded." Purpose-built for offscreen
  rendering with swappable graphics/windowing backends; exports rendered frames
  to a buffer with no display server. This is the right tool. FreeBSD ports:
  `wpewebkit` / `libwpe` / `wpebackend-fdo`.
- **WebKitGTK** — easier to prototype (offscreen via `GtkOffscreenWindow`,
  `webkit_web_view_*` API, lots of docs) but pulls in GTK. Acceptable fallback
  for the first Linux prototype; migrate to WPE for the lean embed. FreeBSD
  ports: `webkit2gtk3`.
- **Gecko** — rejected. Mozilla removed embedding support (~2011–2016);
  EmbedLite/XULRunner are unsupported and fragile against upstream. (SL used
  Gecko once via LLMozLib; that era is dead.)

Recommended path: prototype on **WebKitGTK** for speed, then move the render
path to **WPE** for the shipping embed. The plugin message-handling code is
identical either way; only the engine glue differs.

## Reference implementation

Mirror `indra/media_plugins/cef/media_plugin_cef.cpp` (1266 lines). Its class
`MediaPluginCEF : MediaPluginBase` is the template. `media_plugin_libvlc.cpp`
is a second, simpler example of the same base.

`MediaPluginBase` provides: `mPixels` (shared-memory texture buffer),
`mWidth/mHeight/mDepth` (mDepth = 4, BGRA), `setDirty(l,t,r,b)`,
`sendMessage(LLPluginMessage)`, and the `receiveMessage(const char*)` entry
point the viewer calls.

## The render path (the one hard part)

CEF delivers frames via `onPageChangedCallback(pixels, x, y, w, h)`, which does:

```cpp
if (mWidth == width && mHeight == height)
    memcpy(mPixels, pixels, mWidth * mHeight * mDepth);   // BGRA, depth 4
else
    engine->setSize(mWidth, mHeight);                     // renegotiate size
setDirty(0, 0, mWidth, mHeight);
```

WPE equivalent: run a `wpe_view_backend`, receive each exported frame buffer
(SHM or DMA-BUF via `wpebackend-fdo`), convert to BGRA if needed, `memcpy` into
`mPixels`, then `setDirty`. WebKitGTK equivalent: render into a
`GtkOffscreenWindow`, grab the cairo/GL surface on `draw`, copy to `mPixels`.
The viewer's texture size is authoritative — honor `size_change` by resizing
the WebView, never the reverse.

## Message contract (from `media_plugin_cef.cpp`)

### Incoming (viewer → plugin) → WebKit API

| Message | Action |
|---|---|
| `init` / `size` / `size_change` | create WebView at W×H; on resize, resize view + reallocate expectations; reply `size_change_response` with `texture_width/height` |
| `load_uri` (`uri`) | `webkit_web_view_load_uri` |
| `navigate` / `browse_reload` | `webkit_web_view_reload` |
| `browse_stop` | `webkit_web_view_stop_loading` |
| `browse_back` / `browse_forward` | `webkit_web_view_go_back` / `go_forward` |
| `mouse_event` (down/up/move/double, x,y,button,modifiers) | synthesize pointer event into the view |
| `scroll_event` (x,y,dx,dy) | synthesize axis/scroll event |
| `native_key_data` / `event_keycode` / `virtual_key(_win)` / text | synthesize key events; unicode text input |
| `set_cookie` / `clear_cookies` / `cookies_enabled` | `WebKitCookieManager` |
| `paste` | inject clipboard text |
| `focus` / `set_page_zoom_factor` / `set_user_agent` | view focus, `webkit_web_view_set_zoom_level`, settings |

### Outgoing (plugin → viewer) ← WebKit signals

| Plugin sends | Triggered by |
|---|---|
| `navigate_begin` (uri, `history_back/forward_available`) | `load-changed` = `WEBKIT_LOAD_STARTED` |
| `navigate_complete` (uri, result_code) | `load-changed` = `WEBKIT_LOAD_FINISHED` |
| `location_changed` / `navigated` (uri) | `notify::uri` |
| `name_text` (title) | `notify::title` |
| `status_text` (status) | `mouse-target-changed` / status |
| `file_download` (filename) | `decide-policy` = download |
| `size_change` (`texture_width/height`) | offscreen buffer size negotiation |
| cursor change | `notify::cursor` → map to viewer cursor enum |

Custom-scheme handling (`onCustomSchemeURLCallback` in CEF — for `secondlife://`
and app links) maps to `webkit_web_context_register_uri_scheme` + intercepting
navigation policy; forward as the same message the viewer expects.

## Build wiring

- New dir `indra/media_plugins/wpe/` with `media_plugin_wpe.cpp` +
  `CMakeLists.txt` mirroring `media_plugins/libvlc/CMakeLists.txt`.
- `pkg-config` the engine: `wpe-webkit-2.0` (WPE) or `webkit2gtk-4.1`
  (WebKitGTK). No autobuild package — system lib, which fits the BSD
  `USESYSTEMLIBS` model.
- Register the plugin's MIME types (`text/html`, `application/xhtml+xml`, etc.)
  where CEF's are registered, so the media system selects it for web content.
- Gate by platform: WPE plugin on Linux/BSD, CEF plugin stays on Windows/Mac.
  Both implement the same protocol, so the viewer core is untouched.

## Development sequence (decoupled from the FreeBSD port)

1. **Prototype on Linux** (Firestorm builds on Linux today; WebKit is native;
   CEF is also present for A/B). Get a page rendering to a prim, then input,
   then navigation/cookies. This proves MOAP-on-WebKit independent of BSD.
2. **Move the render path to WPE** for the lean, display-less embed.
3. **On FreeBSD** the plugin comes along for free — WebKit is in ports; only the
   `pkg-config` module name / link flags differ.

## Open risks

- **Input event synthesis** is fiddly (keycodes, modifiers, IME). CEF's
  `native_key_data` path is the reference for exact semantics.
- **DMA-BUF vs SHM** frame export performance under WPE; SHM is simpler, start
  there.
- **Custom schemes / app links** (`secondlife://`, media autoplay policy) need
  care to match CEF behavior users expect.
- **HiDPI / device scale** — honor the viewer's texture size as authoritative.
