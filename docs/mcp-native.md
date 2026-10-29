# Native console capture and SSE

Custom extension of `Rubberbandits/gmod-holylib` at base commit `e13da06e9bc9ad30e98b38c0627930d0a4c71da7`. It requires Linux x86-64 for console capture. The buffered HTTP API remains available. See the parent project's README/SPEC for MCP integration.

## Build

The existing `.github/workflows/compile.yml` selects `RaphaelIT7/garrysmod_common` branch `holylib` and `RaphaelIT7/sourcesdk-minimal` branch `x86-64-patch-3` for Linux 64-bit. Initialize nested dependencies **before** generating projects. This local build used common commit `bea8c322a61b8312c34d4706223d1478cc1aa420` and SDK `4891775e5f65293708b57ae2bc7bde8dd92aea2b`.

From the HolyLib submodule on Linux, with git, a C++17 compiler, make, CMake, and Python 3 installed:

```sh
git clone --branch holylib https://github.com/RaphaelIT7/garrysmod_common.git development/garrysmod_common
git -C development/garrysmod_common checkout bea8c322a61b8312c34d4706223d1478cc1aa420
git -C development/garrysmod_common submodule update --init --recursive
git -C development/garrysmod_common/sourcesdk-minimal fetch origin x86-64-patch-3
git -C development/garrysmod_common/sourcesdk-minimal checkout 4891775e5f65293708b57ae2bc7bde8dd92aea2b
chmod +x development/module/premake5
./development/module/premake5 --gmcommon=development/garrysmod_common --os=linux --arch=x86_64 gmake
make -C projects/linux/gmake config=releasewithsymbols_x86_64 -j4
cmake -S tests/mcp -B _builds/native-linux
cmake --build _builds/native-linux -j4
(cd _builds/native-linux && ctest --output-on-failure)
python3 tests/mcp/manifest.py projects/linux/gmake/x86_64/ReleaseWithSymbols/gmsv_holylib_linux64.so
```

Use the root premake entry point as above: `development/module/premake5.lua` enables an automatic install step. No live-server installation is needed for this build. Generated files are ignored. Optional IVP is handled by the fork's existing generator.

The artifact is `projects/linux/gmake/x86_64/ReleaseWithSymbols/gmsv_holylib_linux64.so`. Archive it with its `.manifest.json`, source commit, and local patch/new files. A dirty build is explicitly identified as `COMMIT-dirty-PATCHHASH`; it is not a binary of the unmodified base commit. Native changes remain uncommitted for review. After committing them, rebuild/test and generate a clean manifest before distributing a commit-pinned artifact. Use a toolchain/runtime compatible with the target SRCDS host; a successful build on recent WSL Ubuntu does not establish compatibility with older distributions. The existing upstream Linux workflow is the reference for release-container dependencies.

The delivered build uses Debian 11 bullseye with GCC 10.2.1. Its versioned runtime symbols require GLIBC 2.30, GLIBCXX 3.4.26, and CXXABI 1.3.9. It has passed the standalone native tests inside that build environment; it has not been installed on SRCDS. The earlier Ubuntu 26.04 development artifact requires GLIBC 2.43 and is not the delivery artifact. For a binary copied from another build environment, pass `--build-info FILE.json` to the manifest generator with that environment's `compiler` and `platform` strings; the manifest also records the ELF symbol-version requirements.

## `consolecapture`

Enable with `-holylib_enable_consolecapture 1`. Registers an additional engine `ILoggingListener`; no listener is replaced and no response policy changes. Messages have channel, syslog-compatible level (`info`, `warning`, `error`), RGBA color, Unix timestamp, text and decimal sequence string. Fatal logging maps to `error`; the engine still applies its usual response. Direct process stdout/stderr is outside the logging interface.

The native mutex-protected ring holds at most 4 MiB of text and 4,096 records, splitting messages into <=8 KiB pieces. Raw engine severity and channel ID are retained alongside the mapped level/name. Logging callbacks copy data only, never invoke Lua. Sequence positions use opaque epoch/sequence/byte-offset cursors, avoiding Lua number precision loss.

| Function | Returns |
| --- | --- |
| `GetCursor()` | Position after current output |
| `Read(cursor?, maxRecords=256, maxBytes=65536)` | Batch table: `records`, `cursor`, `from_cursor`, `reset`, `dropped`, `has_more` |
| `Subscribe(callback, cursor?)` | State-owned numeric subscription ID; omitted cursor starts now |
| `Unsubscribe(id)` | Boolean; affects only the calling state's subscriptions |
| `GetStats()` | Retained bytes/records, limits, cumulative dropped records/bytes, earliest/current cursors |

Read limits: 1–4096 records, 1–1 MiB of text. Omitted read cursor starts at retained history. Invalid/expired cursors resume there with `reset`; `dropped` reports lost records where known. Every record includes its fragment's continuation `cursor`. The batch cursor advances only through returned bytes. Capture history survives Lua shutdown/map change; all Lua subscription references are freed on shutdown. Native module shutdown unregisters the listener and releases history. Subscription dispatch is main-thread-only, round-robin, bounded to 128 KiB per LuaThink and 64 KiB per callback, with up to 128 subscriptions. Callback errors unsubscribe that observer. Thread-local suppression prevents observer print recursion while ordinary console display and other threads' capture continue.

Record splitting and read cursors preserve complete valid UTF-8 code points. `maxBytes` is a target: when the next code point alone is larger, the read returns that one code point (at most four bytes) to ensure progress. Otherwise it stops before a code point that would exceed the budget. Cursors inside a valid code point reset explicitly. Invalid source bytes remain raw for the JSON layer to repair.

## HTTP streaming API

`httpserver.SSE_VERSION == 1` identifies these APIs. Route registration must precede `Start`. Start binds synchronously and fails on bind error. Engine client resolution, proxy interpretation, whitelist checks, prepared response matching, and Lua routes execute on the main thread. Each route is dispatched once. Returning `false` defers a buffered request until `MarkHandled()`; a condition variable wakes its worker. Workers retain shared native storage and never touch engine clients or Lua.

```lua
server:Get("/events", function(request)
    local stream = request:StartSSE()
    stream:OnClose(function(reason) -- owning Lua thread, exactly once
        -- release producer state; do not Stop/Destroy this listener here
    end)
    stream:WriteComment("connected")
    stream:WriteEvent('{"text":"hello"}', "console", "opaque-cursor")
    stream:Close() -- drain, then finish
end, false)
```

`StartSSE(options?)` consumes the request response interface immediately. Later setters/`MarkHandled` are invalid; the route dispatcher recognizes the transition. It preserves other selected headers but replaces content/framing headers with `text/event-stream`, `Cache-Control: no-cache, no-transform`, `X-Accel-Buffering: no`, and identity encoding. The bundled library's compression selection respects identity. Socket workers drain the chunked provider without blocking Lua writes.

| Stream method | Behavior |
| --- | --- |
| `WriteEvent(data, event="message", id?)` | `success, reason?`; UTF-8 required; multiline data normalized/framed; event/ID reject CR/LF/NUL |
| `WriteComment(text)` | Queue UTF-8 comment lines; `success, reason?` |
| `GetBufferedBytes()` | Pending queued bytes, excluding a worker's in-flight socket write |
| `IsOpen()` | Whether new writes are accepted |
| `Close()` | Reject new writes, drain queued output, finish |
| `Abort(reason="aborted")` | Discard queue and terminate |
| `OnClose(callback)` | Register/replace one terminal callback before delivery; returns success |

Defaults: `max_buffered_bytes=262144`, `max_event_bytes=65536` (framed bytes), `heartbeat_seconds=15`. Options allow buffer 64 KiB–8 MiB, event 1 KiB–buffer size, heartbeat 1–60 integer seconds. Write failures include `closed`, `invalid_event`, `invalid_comment`, `too_large`, and `backpressure`; overflow aborts rather than waits. `Close`/`Abort` are idempotent. The first terminal reason wins. Terminal callbacks run during LuaThink or Stop, after worker notification, never on the network thread. Closed userdata safely reports closed/zero; writers return false. Callback references/userdata are released before owner destruction. Stop gives already-closing streams a shared 250 ms drain window, aborts the rest, wakes deferred requests, and joins workers before freeing storage. It delivers terminal callbacks without recursive route/Think dispatch. Stop/Destroy from route/terminal callbacks is rejected; schedule it for a later frame. Lua shutdown prevents new listeners while releasing state.

`SetThreads(workerCount, maxQueuedRequests)` retains its signature: the second argument is the queue limit, **not a maximum thread count**; zero means unlimited queueing. Set it before Start. `SetThreadSleep` is retained as a deprecated compatibility setter; coordination uses notifications, with a 250 ms disconnect check. Long responses each occupy one worker. Producers must bound active streams and retain control workers; the MCP addon supplies those limits.

`SetPayloadMaxLength` bounds decoded bytes before appending them to a request body, including chunked/EOF-delimited transfers and decompression output. Oversized uploads receive HTTP 413 and close the connection without waiting for the remainder of the body. Rebuild the native module to obtain these receive-limit and UTF-8 fixes; the older delivery artifact does not contain them.

Terminal Lua callbacks wait for the provider/resource releaser to finish (or for an unstarted request to be released). Producer abort and network completion are distinct states: an aborted stream can still have an in-flight write. The addon holds its capacity reservation until native completion so slow consumers cannot release a slot early and accumulate extra occupied workers. Socket writes remain subject to the configured write timeout during abort/stop.

## Validation and target acceptance

Standalone tests cover concurrent append/read/eviction, byte fragmentation, loss/reset, cross-thread observer suppression, framed UTF-8, injection, queue overflow, close/abort races, condition-variable wakeup, early/incremental HTTP delivery, heartbeat, disconnect, and stop-before-listen/rebind. Run sanitizer builds where available:

```sh
cmake -S tests/mcp -B _builds/native-asan -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build _builds/native-asan -j4
(cd _builds/native-asan && ctest --output-on-failure)
```

`gluatests/lua/tests/modules/consolecapture/Capture.lua` adds actual engine binding checks for output, fragments, observer suppression, unsubscribe, listener registration/removal, and subscription cleanup on module disable. They require Linux SRCDS and the enabled custom module; they are not part of the standalone executable. Module disable from its own observer/HTTP callback is refused; schedule disable on a later frame. Target acceptance also requires:

1. Run the parent's HTTP/SDK and streaming smoke clients without `-condebug`; verify print, Msg/MsgC, warnings, delayed timer errors, echo and status.
2. Keep multiple MCP/raw observers open while submitting commands. Verify severity opt-in, quiet heartbeats, disconnect/cancel, replay, ring overflow/reset, and slow-reader removal. Inspect `GetStats` and returned continuation cursors.
3. Repeat pending/deferred calls and streams while stopping/restarting listeners, reloading the addon, disabling modules, and changing maps. Check terminal callbacks exactly once, no route redispatch, and no retained Lua references. Capture cursors should retain their epoch across maps and reset after native module disable/re-enable.
4. Exercise a secondary Lua-state teardown if using HolyLua: subscriptions must never be delivered on a worker state, and references must be released at owner shutdown. Run the upstream engine reference/leak checks.
5. Verify empty-server operation with `sv_hibernate_think 1`, bind conflicts, and command availability at the configured stream/async limits. Map/shutdown commands intentionally disconnect clients.

No live server is installed or restarted by this implementation. Until these checks run on the target server, engine coverage and SRCDS lifecycle behavior remain unverified.
