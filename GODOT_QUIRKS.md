# Godot Architectural Quirks, Edge Cases & Real-World Lessons

Last updated: 7 November 2026

Sourced from Godot 4.7 docs and source code.

This document serves as an authoritative reference for developers working on the `godot-directstorage-extension` codebase.

DirectStorage is a low-level Windows / Direct3D 12 subsystem designed for raw NVMe streaming and GPU hardware decompression. However, Godot Engine operates on a specific high-level architecture with strict threading rules, rendering boundaries, and lifecycle contracts.

Applying "theoretical C++ optimizations" without understanding Godot's real engine behavior has previously caused major regressions—including 17-second loading freezes, race conditions with autoloads, and DLL initialization crashes. This guide documents those findings, how Godot actually works, how game developers use it, and how to avoid these pitfalls.

---

## 1. `ResourceLoader` Threading Model & Sequential Asset Traversal

### The Engine Reality
When a game loads a scene in the background via [ResourceLoader.load_threaded_request()](https://docs.godotengine.org/en/stable/tutorials/io/background_loading.html), Godot offloads parsing and loading to worker threads managed by Godot's `WorkerThreadPool`.
- In Godot, [ResourceFormatLoader::load()](https://docs.godotengine.org/en/stable/classes/class_resourceformatloader.html) is a **synchronous, single-item callback**:
  ```cpp
  virtual Variant _load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const;
  ```
- Godot parses the scene file (`level.tscn`) and iterates through external dependencies sequentially:
  ```text
  Worker Thread:
    Calls loader.load("Texture1") ──► BLOCKS until completed Texture2D returned
    Calls loader.load("Texture2") ──► BLOCKS until completed Texture2D returned
    Calls loader.load("Texture3") ──► ...
  ```

### The Fallacy / Anti-Pattern
Trying to perform asynchronous multi-item batching *inside* `ResourceFormatLoader::_load()` on a worker thread is impossible. Godot provides no batch context to the loader; it demands an immediate, finished `Ref<Resource>` return before requesting the next dependency.

---

## 2. `RenderingDevice` Thread Affinity & The 17-Second Hang

### The Engine Reality
In Godot 4.x, [RenderingDevice](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html) methods—most critically `get_driver_resource()` and `free_rid()`—are **strictly thread-affine to the render thread** via `ERR_RENDER_THREAD_GUARD()`. While `texture_create()` itself is protected by internal mutexes, any real GPU texture initialization immediately requires driver resource queries (`get_driver_resource()`) and failure cleanup (`free_rid()`), both of which trigger debug assertions and fail when called from background worker threads (see [Godot Thread-safe APIs](https://docs.godotengine.org/en/stable/tutorials/performance/thread_safe_apis.html)).

### The Trap: Marshaling via `call_on_render_thread()`
To bypass this limitation, an earlier experimental implementation marshaled texture creation and driver queries from worker threads to the render thread using `RenderingServer::call_on_render_thread()`, waiting on a `std::condition_variable`.

In Godot's default "Safe" thread model (`OS::RENDER_THREAD_SAFE` / project setting `rendering/driver/threads/thread_model` = `"Safe"`), **callables posted to the render thread are pushed to a command queue and only drained once per frame when the main thread draws**:
- Texture 1 creation wait: ~16.6 ms (1 frame at 60 FPS)
- Texture 1 driver query wait: ~16.6 ms (1 frame)
- Texture 2 creation wait: ~16.6 ms (1 frame)...

In a real game project like `tps-demo` with ~250 textures:
```text
250 textures * ~60 ms cross-thread wait = ~15,000 ms (15 to 17 seconds of frozen loading screen!)
```

### The Invariant Rule
Direct GPU texture decompression into [Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html) is only viable **on the main or render thread** (synchronous loads or runtime streaming systems).
Worker-thread background loads (`load_threaded_request` with sub-threads) **MUST enforce the thread-affinity gate**:
```cpp
if (!rs || !rs->is_on_render_thread()) {
    // Clean, immediate fallback to CPU GDeflate + DirectStorage BypassIO memory pipeline.
}
```
This CPU memory pathway decompressing into [ImageTexture](https://docs.godotengine.org/en/stable/classes/class_imagetexture.html) takes ~0.5 ms per texture, loading 250 textures in ~125 ms total without a single frame stall.

### Correction (2026-10-07): the Gate Passes on the Main Thread
`RenderingServer::is_on_render_thread()` returns `Thread::get_caller_id() == server_thread` (`servers/rendering/rendering_server_default.h:1212`), and `server_thread` initializes to `Thread::MAIN_ID` (`rendering_server_default.h:82`) — so the gate is TRUE on the main thread and only worker/foreign threads divert. Proven by `project/headless_tests/test_thread_affinity.gd` (windowed D3D12, GTX 1050 Ti): a main-thread sync `ResourceLoader.load()` executed the GPU branch end-to-end (GDeflate dispatch, `STATUS_COMPLETED`, byte-exact mip 0/1 readback), while `load_threaded_request(use_sub_threads=true)` diverted to `ImageTexture` in 0ms. ARCH-01's "100% dead code" verdict therefore applies to worker-thread loads only. The gate above stays as written — it passes main, diverts workers.

---

## 3. Autoload Concurrency & The `DSBatchManager` Race

### The Engine Reality
[Autoload singletons](https://docs.godotengine.org/en/stable/tutorials/scripting/singletons_autoload.html) in Godot (such as `DSBatchManager.gd`) execute in the main thread loop every frame during `_process()`:
```gdscript
func _process(_delta: float) -> void:
    if ctx.get_pending_request_count() > 0:
        ctx.flush_batch_async()
```

### The Race Condition
When a background worker thread enqueues a request into the global `DSContext` singleton and attempts to call `ds_ctx->flush_batch()` synchronously:
1. Worker thread enqueues request `req`. Pending request count is now 1.
2. Main thread ticks `DSBatchManager._process()` between the worker's enqueue and flush calls.
3. `DSBatchManager` detects `pending_request_count > 0` and calls `flush_batch_async()`.
4. The batch is submitted asynchronously into the DirectStorage queue (`req->get_status()` is `STATUS_SUBMITTED`).
5. Worker thread finally calls `ds_ctx->flush_batch()`, which finds 0 pending requests and returns immediately.
6. Worker thread evaluates `req->get_status() == STATUS_COMPLETED`, which is **false**.
7. Spurious warning emitted:
   ```text
   push_warning: DSResourceFormatLoader: GPU texture load failed...; falling back to CPU decompression.
   ```
8. The worker thread prematurely frees the newly allocated GPU texture RID while DirectStorage compute shaders are still writing to it in the background, corrupting memory or leaking resources.

### The Rule
Never enqueue synchronous single-request jobs into an engine-wide shared queue that an autoload is polling and flushing asynchronously. Keep asynchronous batch streaming and synchronous loaders decoupled.

---

## 4. The Transparent Asset Pipeline (`.png` -> `.dstex` -> Runtime)

### How Developers Code Their Games
Game developers in Godot **never author or reference `.dstex` files directly**. They author standard image formats (`.png`, `.jpg`, `.webp`) and assign them to materials, sprites, or 3D meshes:
```gdscript
var tex = load("res://characters/hero.png")
```

### The Importer Hook
1. The extension registers an [EditorImportPlugin](https://docs.godotengine.org/en/stable/classes/class_editorimportplugin.html) (`ds_importer.gd`) with priority `2.0` (overriding Godot's built-in texture importer).
2. The importer converts `res://hero.png` into a hardware-aligned binary container:
   `res://.godot/imported/hero.png-<hash>.dstex`
3. At runtime, Godot's engine parses `hero.png.import` and transparently routes the request to [DSResourceFormatLoader](https://docs.godotengine.org/en/stable/classes/class_resourceformatloader.html) with the `.dstex` path.

### The Gotcha
Any warnings or error logs generated by `DSResourceFormatLoader` will print the imported `.dstex` cache path (e.g., `res://.godot/imported/LightRays.png-6ea...dstex`). Developers can easily confuse this with an asset bug if the relationship between `.png` and `.dstex` is not clearly documented.

---

## 5. `Texture2DRD` vs `ImageTexture` & Engine Compatibility

### Architectural Differences
- [Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html): A thin Godot wrapper around a raw `RenderingDevice` texture RID. Data resides solely in GPU VRAM. It has no CPU-side pixel buffer, and calling CPU readback functions requires manual staging buffers.
- [ImageTexture](https://docs.godotengine.org/en/stable/classes/class_imagetexture.html): A standard engine texture backed by a CPU [Image](https://docs.godotengine.org/en/stable/classes/class_image.html) buffer.

### Engine Edge Cases
Certain Godot rendering features expect `ImageTexture` (or textures providing CPU image data):
- **Light3D Projectors**: Projector textures on OmniLight3D and SpotLight3D require CPU image formats for filtering setup and will push errors if assigned a `Texture2DRD` (explicitly excluded via `-Texture2DRD` property hint in `Light3D`).
- **Decals & Material Pixel Queries**: Custom gameplay scripts or tools querying CPU pixel buffers from materials or decals will receive null images from raw `Texture2DRD` instances unless manual staging readbacks are implemented.
- **`get_image()` Calls**: Gameplay scripts analyzing texture pixels on CPU will receive null images from raw `Texture2DRD` instances.

### The Solution
The importer exposes an import option: `compatibility/as_image_texture`. When enabled, the loader bypasses `Texture2DRD` and always returns an `ImageTexture` loaded via the DirectStorage memory pathway.

---

## 6. GDExtension DLL Lifecycle & Static C++ Initialization (Error 1114)

### The Windows CRT Startup Trap
When Windows loads a dynamic link library (`LoadLibrary`), the C++ runtime initializes all global and file-static objects during `DllMain`.

At this exact moment, Godot's `gdextension_interface` function pointer table is **null**—it is only populated later when Godot invokes `initialize_level()`.

### The Crash
Any static C++ object whose constructor or destructor interacts with Godot types (such as `godot::String`, `godot::Ref<T>`, `godot::Variant`, or `godot::Dictionary`) will attempt to call a null function pointer, immediately crashing with:
```text
LoadLibrary failed: Error 1114 (A dynamic link library (DLL) initialization routine failed).
```

### The Rule
- **NEVER declare file-static or global Godot objects**:
  ```cpp
  // CRASH: invokes godot::String constructor before gdextension_interface is valid!
  static godot::String s_cache_dir;
  static godot::Ref<DSPkg> s_cached_pkg;
  ```
- **Use native C++ types or lazy-initialized heap pointers instead**:
  ```cpp
  // SAFE: standard C++ types require no engine binding
  static std::wstring s_cache_dir;
  static DSPkgCache *s_cached_pkg = nullptr;
  ```

---

## 7. `godot::RID` and C++ Standard Library Containers

### The Compilation Error
In `godot-cpp`, `godot::RID` does not provide an out-of-the-box specialization for `std::hash<godot::RID>`. Writing:
```cpp
std::unordered_map<godot::RID, ID3D12Resource *> cache; // MSVC Error C2064
```
causes MSVC compiler errors (`term does not evaluate to a function taking 1 arguments`).

### The Solution
Use the 64-bit integer ID returned by `RID::get_id()`:
```cpp
std::unordered_map<int64_t, ID3D12Resource *> cache;
int64_t rid_key = destination_rid.get_id();
```
`int64_t` has built-in hash support across all STL implementations and avoids custom hasher boilerplate.

---

## 8. DirectStorage Multi-Mip Splitting & D3D12 Hardware Alignments

### The Mip Stream Discrepancy
DirectStorage's `DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE` destination expects a single, continuous, uncompressed stream covering all subresources.

However, in `.dstex` and `.dspkg` containers, **each mip level is an independently compressed GDeflate stream**. DirectStorage cannot decompress multiple independent GDeflate streams into a single multi-subresource destination range.

### The Splitting Solution
Multi-mip textures must be split into separate single-subresource requests (`NumSubresources = 1`) targeting each subresource index individually.

### Hardware Alignment Invariants
Direct3D 12 hardware strictly mandates:
- **Row Pitch**: 256-byte alignment (`D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`).
- **Subresource Placement**: 512-byte alignment (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`).

Both `.dstex` (v2) and `.dspkg` containers pad each mip payload to 512-byte boundaries. This allows DirectStorage unbuffered NVMe DMA transfers to read directly into D3D12 subresource footprints.

### Completion Guard Validation Caching
When a 13-mip texture is split into 13 child requests, all 13 requests share the same destination `RID`. Without caching, post-batch validation queries the render thread 13 times for the exact same resource. Caching the verified resource handle by `int64_t` RID ID within `poll_batch_status()` eliminates redundant cross-thread round-trips.

---

## 9. Headless Automated Testing with Real D3D12 Hardware

### The `--headless` Trap
Passing `--headless` to the Godot binary forces headless display and audio dummy drivers (`main.cpp`), initializing the `RasterizerDummy` rendering driver. Note that `RasterizerDummy` sets `low_end = false`, so querying `RenderingServer::is_low_end()` does not detect it (check the driver/method name instead). Under headless mode, D3D12 device creation is completely bypassed, and any automated test asserting real DirectStorage GPU decompression or VRAM readback will fail.

### The Headless Recipe
To run automated tests on headless CI/CD runners with real D3D12 hardware:
```powershell
& Godot_v4.7.2-stable_win64_console.exe `
    --path <project_path> `
    --rendering-driver d3d12 `
    --audio-driver Dummy `
    --windowed `
    --resolution 64x64 `
    -s headless_tests/<test_script>.gd
```

### Main Thread Frame Pumping
In Godot's single-safe thread model, any worker thread awaiting render-thread callbacks requires the main test script to actively pump frames:
```gdscript
while worker_thread.is_alive():
    await process_frame
```
Without `await process_frame`, the main loop never draws or drains render-thread queues, resulting in an indefinite deadlock.

---

## 10. Windows DLL File Locking During Hot-Reload

### The Problem
When Godot Editor or a test project is running on Windows, the operating system locks loaded binaries (`directstorage_accelerator.dll`, `dstorage.dll`). Standard `Copy-Item` operations fail with:
```text
The process cannot access the file because it is being used by another process.
```

### The Safe-Rename Pattern
Windows allows renaming a running DLL even while it is actively mapped into memory. Build scripts (`.vscode/build_remote.ps1`) should implement the safe-rename pattern:
1. Attempt `Copy-Item -Path $src -Destination $dst -Force -ErrorAction Stop`.
2. On failure, catch the exception, rename the locked destination:
   `Move-Item -Path $dst -Destination "$dst.old" -Force`
3. Copy the newly compiled DLL to the original `$dst` path.
4. Clean up `.old` files on subsequent build iterations.

---

## 11. Compatibility Renderer (OpenGL) & Null `RenderingDevice`

### The Engine Reality
Godot supports multiple rendering methods: Forward+, Mobile (Vulkan/D3D12), and Compatibility (OpenGL/ANGLE / WebGL).
According to the official [RenderingServer.get_rendering_device()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class-renderingserver-method-get-rendering-device) specification:
> *"When using the OpenGL rendering driver or when running in headless mode, this function always returns `null`."*

### The Consequence for DirectStorage
DirectStorage hardware decompression requires Direct3D 12 (`ID3D12Device` / `ID3D12Resource`). When a project runs under the Compatibility renderer, or when `--rendering-driver opengl3` is selected:
1. `RenderingServer::get_singleton()->get_rendering_device()` returns `nullptr`.
2. Direct GPU texture creation into `Texture2DRD` is physically unavailable because there is no `RenderingDevice` or D3D12 device.

### The Defensive Guard Rule
Any code path interacting with `RenderingDevice` must defensively check:
```cpp
RenderingServer *rs = RenderingServer::get_singleton();
if (!rs || !rs->get_rendering_device()) {
    // Graceful fallback to CPU GDeflate + BypassIO / standard file loader
}
```
Failing to verify `get_rendering_device()` will result in immediate null pointer dereferences or crashes when developers switch their project to Compatibility mode.

---

## 12. GDExtension Initialization Levels & ClassDB Invariants

### The Engine Reality
In Godot 4, custom GDExtension classes are registered with [ClassDB](https://docs.godotengine.org/en/stable/classes/class_classdb.html) during specific initialization stages ([GDExtension Initialization Levels](https://docs.godotengine.org/en/stable/tutorials/scripting/gdextension/gdextension_cpp_example.html)):
- `MODULE_INITIALIZATION_LEVEL_CORE`: Low-level primitives.
- `MODULE_INITIALIZATION_LEVEL_SERVERS`: Server singletons and low-level servers.
- `MODULE_INITIALIZATION_LEVEL_SCENE`: High-level node classes, resources, and loaders (`ResourceFormatLoader`, `Texture2D`).
- `MODULE_INITIALIZATION_LEVEL_EDITOR`: Editor plugins (`EditorPlugin`, `EditorImportPlugin`).

### Registration Contracts
- Registering classes inheriting from `RefCounted`, `Resource`, or `ResourceFormatLoader` must occur at `MODULE_INITIALIZATION_LEVEL_SCENE`. Registering them too early (e.g. CORE) causes undefined behavior because base engine types are not yet registered.
- Clean deinitialization must unregister resources in reverse order during shutdown.
- When binding methods via `ClassDB::bind_method(D_METHOD("method_name", "arg1"), &MyClass::method_name)`, method parameter counts and default values ([DEFVAL](https://docs.godotengine.org/en/stable/classes/class_classdb.html)) must strictly match between C++ and GDScript. Mismatched signatures can cause silent invocation failures or engine crashes.

### The Re-Entrancy Trap: `register_class` Inside `_bind_methods()`
`ClassDB::register_class<A>()` invokes `A::_bind_methods()` midway through mutating the class registry. Calling `ClassDB::register_class<B>()` from inside `A::_bind_methods()` therefore re-entrantly mutates `ClassDB::classes` while registration of `A` is still in flight — undefined behavior that can corrupt the registry and segfault the engine at extension load (observed as a `signal 11` crash in the macOS editor at `directstorage_accelerator_init` time):
```cpp
// CRASH-PRONE: re-entrant registry mutation mid-registration of DSProfiler!
void DSProfiler::_bind_methods() {
    ClassDB::register_class<DSProfilerResult>();
    ...
}
```

### The Rule
- Register every class exactly once, explicitly, in the module initializer (`initialize_<name>_module`), in dependency order — dependencies before dependents:
```cpp
// SAFE: flat, ordered registration at MODULE_INITIALIZATION_LEVEL_SCENE
ClassDB::register_class<DSProfilerResult>(); // dependency first
ClassDB::register_class<DSProfiler>();
```
- **NEVER call `ClassDB::register_class` from inside any `_bind_methods()`**. `_bind_methods()` is for `bind_method`/`bind_integer_constant`/property calls only.

---

## 13. Time-Sliced Scene Prewarming & The VSync "Yield Tax"

### The Engine Reality
Because Godot's `RenderingDevice` requires strict render-thread affinity (see Section 2), running `ResourceLoader.load_threaded_request()` on background worker threads forces the DirectStorage loader to bypass GPU decompression and fall back to CPU GDeflate decompression into `ImageTexture`.

To achieve true zero-copy GPU decompression into [Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html) on D3D12 hardware without freezing the main thread or loading screen UI, developers can perform **time-sliced prewarming** on the main thread:
```gdscript
for path in texture_paths:
    ResourceLoader.load(path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
    await get_tree().process_frame
```

### The Pitfall: The VSync "Yield Tax"
In Godot, `await get_tree().process_frame` synchronizes directly with the window display refresh rate (VSync). At a standard 60 Hz refresh rate, each frame tick takes **16.6 ms**.

If a naive prewarming script loads only 1 texture per frame:
```text
80 textures * 16.6 ms per frame = 1,328 ms (~1.33 seconds)
```
In this scenario, over 95% of total loading screen time is spent idling waiting for the monitor's VSync pulse, even though DirectStorage NVMe I/O and GPU hardware decompression executed in under 0.5 ms per texture. In contrast, background worker threads in `load_threaded_request()` do not yield to VSync and process requests continuously at full CPU speed.

### The Solution: Frame-Budgeted Prewarming
Instead of yielding every single texture, scripts must implement microsecond frame budgeting via [Time.get_ticks_usec()](https://docs.godotengine.org/en/stable/classes/class_time.html#class-time-method-get-ticks-usec):
```gdscript
var idx := 0
var total := texture_paths.size()
while idx < total:
    var frame_start := Time.get_ticks_usec()
    # Consume up to 8-12 ms per frame, leaving headroom for 60+ FPS UI rendering
    while idx < total and ((Time.get_ticks_usec() - frame_start) < 8000 or idx == 0):
        ResourceLoader.load(texture_paths[idx], "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
        idx += 1
    loading_progress.value = (float(idx) / float(total)) * 100.0
    await get_tree().process_frame
```
By batching 4 to 8 textures within an 8 ms frame budget, the total frame yields drop from 80+ frames down to ~15 frames. This eliminates over 1 second of idle VSync delay while maintaining a buttery-smooth 60 FPS loading animation.

---

## 14. Cross-Thread `ResourceCache` Deduplication (`CACHE_MODE_REUSE`)

### The Engine Reality
Godot maintains an internal, engine-wide `ResourceCache` map storing active `Resource` references keyed by path. In Godot 4.x, queries to `ResourceCache` are thread-safe across the main thread and `WorkerThreadPool` background threads.

### The Interaction with DirectStorage
When textures are pre-warmed on the main render thread via `ResourceLoader.load(..., CACHE_MODE_REUSE)`, they enter `ResourceCache` as resident `Texture2DRD` instances.

When the parent `PackedScene` (`level.tscn`) is subsequently loaded:
- Whether loaded synchronously on the main thread via `ResourceLoader.load()`
- Or asynchronously on a worker thread via `ResourceLoader.load_threaded_request()`

Godot's internal scene dependency resolver queries `ResourceCache::has(dep_path)`:
1. It hits the resident cache for each texture dependency.
2. It completely skips disk I/O and skips re-invoking `ResourceFormatLoader::_load()`.
3. It binds the existing, GPU-resident `Texture2DRD` pointer directly into the scene's materials and mesh surfaces.

In empirical testing with `tps-demo` across 55 materials and 100+ mesh surfaces:
- 53 out of 55 mesh textures bound as pointer-identical `Texture2DRD` instances.
- 0 disk re-reads occurred during the scene load phase.

### The Trap: `CACHE_MODE_IGNORE` / `CACHE_MODE_REPLACE`
Passing `ResourceLoader.CACHE_MODE_IGNORE` or `CACHE_MODE_REPLACE` when loading the parent scene forces Godot to bypass `ResourceCache`. If the scene is loaded on a worker thread, this triggers redundant disk reads and falls back to CPU decompression into `ImageTexture`, completely wasting the prewarm effort and allocating duplicate textures in memory.

---

## 15. The Pipeline Serialization Penalty (Prewarm + Threaded vs. Prewarm + Sync)

### The Anti-Pattern
Developers often attempt to combine techniques: *"Prewarm textures on the main thread with DirectStorage to get `Texture2DRD`, and then call `ResourceLoader.load_threaded_request()` for the scene so the main thread never blocks."*

This introduces a severe **pipeline serialization penalty**:
```text
Approach A: Standard Threaded Load (Single Pass)
  Worker Thread: [Parse Scene + Load Textures via CPU Decompression] ──► ~3.17 s total

Approach B: DS Prewarm + Threaded Scene Load (Serialized Two-Pass)
  Pass 1 (Main Thread):   [Time-Sliced Texture Prewarming]           ──► ~2.50 s
  Pass 2 (Worker Thread): [load_threaded_request() Scene Parse]     ──► ~1.66 s
  Total Wall-Clock Time:                                            ──► ~4.16 s (+1.0 s slower!)

Approach C: DS Prewarm + Synchronous Scene Load (Optimized DirectStorage)
  Pass 1 (Main Thread):   [Time-Sliced Texture Prewarming]           ──► ~2.30 s
  Pass 2 (Main Thread):   [ResourceLoader.load() Cached Scene Parse] ──► ~0.66 s
  Total Wall-Clock Time:                                            ──► ~2.96 s (Fastest & 100% Texture2DRD)
```

### The Architectural Reason
In Approach B, `load_threaded_request()` incurs thread dispatch overhead, worker thread synchronization, and incremental dependency polling across frames even when textures are already cached.
In Approach C, because all heavy texture I/O is already complete, `ResourceLoader.load(LEVEL_PATH, "PackedScene", CACHE_MODE_REUSE)` on the main thread only needs to parse the node hierarchy and resolve cached pointers, finishing in a fraction of a second while achieving full GPU residency.

---

## 16. Scene Instantiation (`instantiate()`) & PSO Shader Compilation Lag

### The Engine Reality
DirectStorage accelerates storage-to-VRAM throughput and hardware asset decompression. However, it does **not** bypass Godot's CPU-side scene graph instantiation or GPU driver Pipeline State Object (PSO) compilation.

### The Two Post-Load Latency Phases
When benchmarking level transitions, developers must account for two distinct post-load phases:
1. **Scene Instantiation (`PackedScene.instantiate()`)**:
   - `instantiate()` builds the Node tree hierarchy, initializes properties, and sets up node structures.
   - In a production-scale scene like `tps-demo`, `instantiate()` takes **~140 ms to ~160 ms** of synchronous CPU execution time.
2. **First-Frame PSO Compilation & Buffer Upload (`add_child()`)**:
   - When the newly instantiated scene is added to the active `SceneTree` and drawn for the first time, the graphics driver must compile pipeline state objects (PSOs), link vertex/fragment shaders, and upload vertex/index buffers to the GPU.
   - This creates an initial frame time spike of **~380 ms to ~450 ms** during which the game loop lags before settling into a smooth 60+ FPS cadence. Engine maintainer analysis ([#114959](https://github.com/godotengine/godot/issues/114959)) confirms these stalls are synchronous driver pipeline creation waits in `ParticlesStorage` and `material_create_from_shader()` (`material.cpp:2089`) blocking on `version_is_valid`.

### The C++ Tree-Guard Mechanism (`packed_scene.cpp` vs `node.cpp`)
Why does `instantiate()` pass engine thread checks while `add_child()` strictly fails?
- **Off-Tree Instantiation Bypasses Tree Guards**: `PackedScene::instantiate()` constructs the node subtree using `parent->_add_child_nocheck(...)` (`scene/resources/packed_scene.cpp:543`), not `Node::add_child()`. In `scene/main/node.cpp:1664-1703`, `_add_child_nocheck` only binds trees and propagates notifications `if (data.tree)`. Because a newly instantiated scene root has `data.tree == nullptr`, no `_set_tree()`, `_propagate_enter_tree()`, or `_propagate_ready()` calls are executed.
- **The Guard Invariant (`node.h:694-699`)**:
  ```cpp
  bool is_accessible_from_caller_thread() const {
      return !data.tree || is_current_thread_safe_for_nodes();
  }
  ```
  Disconnected/off-tree nodes pass `ERR_THREAD_GUARD` on any thread. On-tree nodes strictly fail via `ERR_FAIL_COND_MSG(data.tree && !Thread::is_main_thread(), ...)` across `add_child()` (`node.cpp:1712`), `add_sibling()` (`:1736`), `remove_child()` (`:1748`), and `move_child()` (`:504`).
- **`Node` Opts Out of Signal Mutexes (`node.h:426`)**:
  ```cpp
  virtual bool _uses_signal_mutex() const override { return false; } // Node uses thread guards instead
  ```
  `Node` deliberately avoids signal mutex synchronization for performance, relying entirely on the main-thread rule for tree modifications.

### The 5 Traps of Threaded `instantiate()`
While Godot's documentation permits off-tree instantiation, attempting it naively in production exposes several documented engine issues:
1. **`AudioStreamPlayer` Heap Corruption & Thread Race ([#80581](https://github.com/godotengine/godot/issues/80581), [#34752](https://github.com/godotengine/godot/issues/34752))**:
   `AudioStreamPlayer`, `AudioStreamPlayer2D`, and `AudioStreamPlayer3D` constructors invoke `AudioServer::connect(...)` during instantiation (`audio_stream_player_internal.cpp:365-366`). Because `Node` has no signal mutex (`_uses_signal_mutex() == false`), calling `instantiate()` off-thread races on `Object::connect` and can corrupt the heap. Scenes containing audio players must either have them stripped/deferred or be instantiated on the main thread.
2. **Concurrent `instantiate()` Crash on the Same Scene ([#98179](https://github.com/godotengine/godot/issues/98179))**:
   Calling `instantiate()` concurrently on the *same* `PackedScene` resource across multiple worker threads triggers an unref race in `SafeRefCount` inside `SceneState::instantiate`. Threaded instantiation must be strictly single-worker.
3. **Cross-Thread Guard Spam ([#79194](https://github.com/godotengine/godot/issues/79194))**:
   Loading a resource on Thread A and instantiating it on Thread B triggers readable thread-guard spam on nodes like `TileMap` (`TileMap.get_use_parent_material`, `get_transform`). Assets must be loaded and instantiated on the *same* background thread.
4. **Physics Compound Assignment Failure ([#83496](https://github.com/godotengine/godot/issues/83496))**:
   Mutating off-tree physics nodes in a worker thread via compound assignment (e.g., `area.position += offset`) fails because the property getter asserts `ERR_READ_THREAD_GUARD`. Direct property assignment (`area.position = new_pos`) succeeds.
5. **`ClassDB` Telemetry Race ([#122481](https://github.com/godotengine/godot/issues/122481))**:
   `ClassDB::class_get_default_property_value` has an open telemetry race condition when called from worker threads.

### The UI Perception Trap
If a loading screen is dismissed immediately upon resource completion (`ResourceLoader.THREAD_LOAD_LOADED`):
- The player will see a frozen frame of the *old* scene (e.g., the main menu 3D background) while the engine blocks for ~150 ms on `instantiate()` and ~400 ms on first-frame PSO compilation.
- To the player, the game appears to "stutter, freeze, and lag" after the loading screen is already gone.

### The Best Practice
1. **Single-Worker `instantiate()` + Deferred `add_child()`**:
   To eliminate the ~150 ms instantiation freeze from the main thread, instantiate on a dedicated background thread and defer tree attachment:
   ```gdscript
   # Dedicated worker thread (single loader thread only):
   var inst = packed_scene.instantiate()
   world_root.call_deferred("add_child", inst)
   ```
2. **Chunk Large Scenes to Avoid Atomic `add_child()` Freezes ([#102665](https://github.com/godotengine/godot/issues/102665))**:
   Attaching a 1,000+ node tree via `add_child()` is atomic and will hitch the main thread even when deferred. Decompose large environments into room/sub-scene chunks, instantiate each in the worker thread, and attach them one per frame using the 8 ms frame-budgeting pattern (Section 13).
3. **PSO Compilation Mitigations ([#114959](https://github.com/godotengine/godot/issues/114959))**:
   - Collapse branching options in `ParticleProcessMaterial` and custom shaders into uniform booleans to reduce combinatorial PSO variants.
   - Keep the loading screen overlay visible across `instantiate()`, `add_child()`, and at least one drawn frame (`await get_tree().process_frame`) to absorb driver PSO creation before revealing gameplay.

---

## 17. Scene Dependency Traversal vs. Directory Scanning for Prewarming

### The Engine Reality
To prewarm a scene, scripts typically call [ResourceLoader.get_dependencies()](https://docs.godotengine.org/en/stable/classes/class_resourceloader.html#class-resourceloader-method-get-dependencies):
```gdscript
var deps: PackedStringArray = ResourceLoader.get_dependencies(scene_path)
```

### The Incomplete Dependency Trap
`get_dependencies()` only returns dependencies explicitly declared in the `.tscn` or `.tres` text headers (`[ext_resource ...]`).
It does **not** reliably discover:
- Textures referenced dynamically inside GDScript code via string paths (`load("res://textures/foo.png")`).
- Sub-resources embedded within imported 3D glTF/GLB models or external material libraries that haven't been opened yet.
- Secondary effect textures loaded conditionally (e.g., muzzle flashes, blood decals, UI icons).

### The Solution: Hybrid Traversal & UID Normalization
A robust prewarming routine should combine recursive dependency traversal with directory scanning:
1. **Normalize Dependency Strings**: In Godot 4.x, `ResourceLoader.get_dependencies()` returns compound entries:
   - `uid://<id>::<type>::<fallback_path>`
   - `uid://<id>::::<fallback_path>`
   - `<path>::<type>`
   - `uid://<id>`
   Passing these raw compound strings to `ResourceLoader.exists()`, `FileAccess.file_exists()`, or custom filters treats the string as a UID (due to `uid://` prefix) but fails UID integer parsing due to `::` delimiters, causing Godot C++ to log:
   ```text
   Condition "p_id == INVALID_ID" is true. Returning: String()
   core/io/resource_uid.cpp:196 @ get_id_path()
   ```
   Furthermore, `ResourceLoader.exists()` returns `false`, causing traversal loops to prematurely prune nested subscenes. Callers must extract the fallback path (section 3 via `.get_slice("::", 2)`) or safely resolve `uid://` via `ResourceUID.has_id()` / `ResourceUID.get_id_path()` before testing existence or recursing into subscenes.
2. Recursively walk `ResourceLoader.get_dependencies()` for `.tscn`, `.tres`, and `.material` files to capture explicit tree dependencies.
3. Use [DirAccess](https://docs.godotengine.org/en/stable/classes/class_diraccess.html) to scan known level asset directories (e.g. `res://level/textures`, `res://player/textures`) to catch orphaned or dynamic assets.
4. Filter out format-incompatible textures before prewarming—specifically:
   - Light projector textures (`spotlight_projector`) which require CPU `ImageTexture` (Section 5).
   - EXR radiance maps or HDR environment textures requiring specific float formats.

---

## 18. Windows `RenderingDevice` Driver Defaults & Silent Fallbacks

### The Engine Reality
On Windows, Godot's default `RenderingDevice` driver is Vulkan, not Direct3D 12 (`main/main.cpp:2388-2389`):
```text
rendering/rendering_device/driver.windows = "vulkan"
```
In addition, `rendering/rendering_device/fallback_to_vulkan` and `rendering/rendering_device/fallback_to_d3d12` both default to `true` (`main/main.cpp:2396-2397`).

### The Silent Fallback Hazard
During initialization in `platform/windows/display_server_windows.cpp:8041-8059`, Godot iterates through candidate rendering drivers. If D3D12 fails to initialize (or if the developer omitted explicit driver configuration), Godot silently falls back to Vulkan without crashing or emitting an error.

Because DirectStorage GPU hardware decompression requires D3D12 (`ID3D12Device` / `ID3D12Resource`), a silent fallback to Vulkan completely disables all DirectStorage GPU streaming pipelines at startup, causing assets to silently divert to CPU decompression.

### The Invariant Rules
1. Always explicitly pin `rendering/rendering_device/driver.windows="d3d12"` in `project.godot`.
2. For dedicated DirectStorage production builds, set `rendering/rendering_device/fallback_to_vulkan=false` to ensure configuration failures fail loudly rather than degrading silently.
3. At application startup, assert both:
   ```gdscript
   RenderingServer.get_current_rendering_driver_name() == "d3d12"
   DSContext.get_singleton().is_gpu_decompression_enabled()
   ```

---

## 19. Single-`ID3D12Device` Rule & Engine Handle Borrowing

### The Architectural Principle
The DirectStorage GDExtension never creates its own Direct3D 12 device (`D3D12CreateDevice`). Instead, it borrows Godot's active logical device handle via:
```cpp
uint64_t dev_ptr = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
ID3D12Device *device = reinterpret_cast<ID3D12Device *>(dev_ptr);
```

### Why Separate Devices Break DirectStorage
Creating a second `ID3D12Device` inside an extension creates separate VRAM allocation heaps, separate command queues, and distinct synchronization fence domains. Transferring GPU textures between two independent devices requires expensive cross-adapter copies or CPU staging buffers, completely defeating DirectStorage's high-speed GPU decompression advantage.

### The Invariant Rule
D3D12 device, command queue, and resource handles flow **engine -> extension only**, never the reverse.

---

## 20. Direct3D 12 State Ownership & Barrier Exclusivity

### The Engine Reality
DirectStorage GPU destination requests (`DSTORAGE_DESTINATION_TEXTURE_REGION` / `DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE`) require destination `ID3D12Resource` allocations to be in `D3D12_RESOURCE_STATE_COMMON` (`dstorage.h:404-406`). However, the `ID3D12Resource` COM interface exposes no current-state query method.

### The Hazard: Extension-Side Resource Barriers
Only Godot's `RenderingDevice` tracks and manages resource state transitions. If the extension attempted to record its own `ResourceBarrier` commands, it would race with Godot's per-frame render command list recording and corrupt engine state tracking.

### The Defensive Contract
As implemented in `src/platform/win32/ds_win32_backend.cpp:1823-1848`:
1. The extension tracks destination resources in flight (`s_resource_guards`).
2. If a new request targets an `ID3D12Resource` that is currently in flight, the request is rejected immediately with an error rather than attempting speculative barriers or silent CPU fallback.
3. The extension never transitions destination resource states directly; it relies on Godot's render thread to transition textures after completion fences signal.

---

## 21. Virtual Filesystem vs. Raw NVMe Paths (`.pck` vs. `.dspkg` Sidecars)

### The Engine Reality
DirectStorage's Windows API (`IDStorageFactory::OpenFile`) bypasses high-level user-mode file systems and speaks directly to the Windows storage subsystem using Win32 kernel `HANDLE`s and absolute NTFS/NVMe file paths.

### The `.pck` File Limitation
Assets bundled inside Godot's default `.pck` file (`res://`) are virtual sub-allocations packed into a single archive file. They have no individual OS file handles or sector alignments on disk, making it physically impossible for DirectStorage BypassIO to read from them directly.

### The Exporter Solution
DirectStorage textures cannot be packed inside the standard Godot `.pck`:
1. The custom export plugin (`ds_export_plugin.gd:162-166`) calls `skip()` on texture payloads during export, stripping raw binary data out of the PCK while preserving `.import` remap tables.
2. Texture payloads are bundled into loose sidecar container archives (`textures.dspkg`) deployed alongside the executable.
3. Any new DirectStorage asset container must follow the same skip + sidecar pattern.

---

## 22. Foreign `std::thread`s and Fail-Closed Engine Gates

### The Engine Reality
The extension's background completion and fence-monitoring routines execute on native C++ `std::thread`s (`ds_win32_backend.h`, `ds_fallback_backend.h`), which are not created by Godot's `Thread` system.

### How Godot Identifies Foreign Threads
In `core/os/thread.cpp:40-41`:
```cpp
SafeNumeric<uint64_t> Thread::id_counter(1);
thread_local Thread::ID Thread::caller_id = Thread::id_counter.increment();
```
Whenever an unmanaged OS thread touches Godot code that evaluates `Thread::get_caller_id()`, Godot allocates a fresh unique ID from the atomic counter (> 1).

### The Architectural Guarantee
Because `caller_id` is never `MAIN_ID` (`1`) and never matches the render thread's ID:
- `Thread::is_main_thread()` evaluates to `false`.
- `RenderingServer::is_on_render_thread()` evaluates to `false`.

All engine thread-affinity guards automatically **fail closed** on foreign backend threads. Furthermore, because `Node` explicitly opts out of signal mutexes (`node.h:426`: `_uses_signal_mutex() == false`), attempting to invoke node methods or signal connections from foreign threads risks memory corruption rather than safe lock contention (see Section 16). Do not attempt to "adopt" background worker threads into Godot's registry without auditing every single guard.

---

## 23. Deferred `emit_changed()` Delivery During Threaded Loads

### The Engine Reality
When loading resources on worker threads via `ResourceLoader.load_threaded_request()`, calling `Resource::emit_changed()` does **not** trigger signal connections synchronously.

### The Engine Mechanism
In `core/io/resource.cpp:54-57`:
```cpp
if (ResourceLoader::is_within_load() && !Thread::is_main_thread()) {
    ResourceLoader::resource_changed_emit(this);
    return;
}
```
Off-thread notifications are routed to `ResourceLoader::resource_changed_emit()`, which records the callable under `thread_load_mutex` and defers execution to the main thread loop.

### The Rule
Custom loaders and resource types constructed in worker threads must not rely on synchronous `changed` signal execution; all listeners must be prepared for deferred delivery.

---

## 24. Nested `ResourceLoader::load()` Re-Spawning in Worker Threads

### The Engine Reality
If code executing inside a `WorkerThreadPool` background task invokes `ResourceLoader::load()` synchronously, Godot detects that the calling thread is already an active pool worker (`core/io/resource_loader.cpp:730-737`):
```cpp
if (WorkerThreadPool::get_singleton()->get_caller_task_id() != WorkerThreadPool::INVALID_TASK_ID) {
    thread_mode = LOAD_THREAD_SPAWN_SINGLE;
}
```

### The Architectural Behavior
Instead of executing the nested load inline on the current thread, Godot re-spawns it as a separate engine-owned task. This guarantees that all pool-task resource loads are initiated and tracked by the engine, simplifying cyclic load detection and task awaiting.

### The Rule
Recursive loads from worker threads do not deadlock, but task identity changes. Scripts and loaders must not assume `get_caller_task_id()` continuity across nested load calls.

---

## 25. `ImageTexture` Worker-Safety by Construction

### The Engine Reality
As detailed in Section 2, `Texture2DRD` cannot be constructed on background worker threads because `get_driver_resource()` and `RenderingDevice::free_rid()` require render-thread affinity. In contrast, the CPU fallback path decompressing into [ImageTexture](https://docs.godotengine.org/en/stable/classes/class_imagetexture.html) is safe on worker threads by construction.

### The Engine Mechanism
1. An [Image](https://docs.godotengine.org/en/stable/classes/class_image.html) buffer resides purely in CPU RAM and has no thread affinity constraints.
2. When passed to `ImageTexture::set_image()` on a worker thread (`image_texture.cpp:82`), it delegates to `RenderingServer::texture_2d_create()`.
3. In `RenderingServerDefault::texture_2d_create()` (`servers/rendering/rendering_server_default.h:147-150`), Godot allocates an RID immediately and marshals the underlying GPU texture initialization through the `command_queue` when called off the server thread:
   ```cpp
   command_queue.push(RSG::texture_storage, &RendererTextureStorage::texture_2d_initialize, ret, p1);
   ```

### The Rule
Constructing and populating `Image` and `ImageTexture` on background worker threads is completely free of thread-affinity hazards, justifying its use as the authoritative, stall-free fallback pathway during threaded scene loads.

---

## 26. `RenderingDevice::texture_create` Demands a Single Packed Blob for 2D

### The Engine Reality
When creating a 2D texture through [RenderingDevice.texture_create()](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class-renderingdevice-method-texture-create), Godot requires the `data` array to contain **exactly one** `PackedByteArray** holding all mip levels concatenated into a single continuous buffer (`servers/rendering/renderer_rd/rendering_device.cpp:1636-1638`):
```cpp
ERR_FAIL_COND_V_MSG(p_format.mipmaps <= 1 && data.size() != 1, RID(), ...);
ERR_FAIL_COND_V_MSG(data.size() != 1, RID(), "Invalid length (" + itos(data.size()) + ")");
```

### The Trap: One Array Per Mip
A natural implementation passes one array element per mip level (8 mips -> 8 array entries), mirroring how the container stores independently compressed mip streams. The driver rejects this outright with `invalid length (8)`, and the texture silently falls back to the CPU `ImageTexture` path—leaving GPU decompression permanently unproven with zero in-run errors pointing at the real cause.

### The Invariant Rule
Concatenate all mip payloads into one tight `packed_init` blob (bounds-checked, overflow-checked) and call `texture_create(fmt, view, { packed_init })` exactly once for 2D textures. The per-mip splitting required by DirectStorage destinations (Section 8) applies to *requests*, never to the `texture_create` data array.

---

## 27. `Texture2DRD` Is a Non-Owning View: Dropping the Last `Ref` Leaks the RID

### The Engine Reality
[Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html) is a thin wrapper around a raw `RenderingDevice` texture RID. Its destructor **never** frees the underlying device texture. Consequently, every cache-side owner-loss—evict, `clear()`, capacity replacement, disable, shutdown—must explicitly release the RID or it leaks until device teardown. In production profiling this scaled with GPU-resolve count (hundreds of RIDs held at shutdown) with zero in-run errors and flat memory deltas, making it invisible to every probe except a shutdown census.

### The Release Discipline
As implemented in `src/integration/ds_texture_cache.cpp`:
1. Hold the evicted `Ref` locally until its RID is freed inline or queued— the RID value itself survives `Texture2DRD` death, so the handle stays valid after the wrapper dies.
2. `RenderingDevice::free_rid()` asserts the render thread (`ERR_RENDER_THREAD_GUARD`, see Section 2), so off-render-thread callers must queue the RID and post a drain worker via `RenderingServer::call_on_render_thread()`. Missing or dead singletons (headless Dummy driver, module teardown) skip the free—no live device means no live GPU resource.
3. Remove the map entry *before* freeing (idempotent: a repeat evict finds nothing and frees nothing), and gate every free with `texture_is_valid()`—a re-stored evicted texture still carries its stale handle, and a second free spams `Attempted to free invalid ID`.
4. Perform all of the above **outside** the cache mutex (`free_rid` takes `RenderingDevice` locks; the deferred path crosses threads).

### Second Owner: Uncached Textures (`DSTextureOwner`, #0126)
Ignored cache modes (`CACHE_MODE_IGNORE`/`IGNORE_DEEP`) and a disabled cache never reach `store()`, so the cache cannot be the owner there. Those textures carry a `DSTextureOwner` (`src/integration/ds_texture_owner.{h,cpp}`) instead: attached through Object metadata, armed with the device RID, released by its destructor through `DSTextureCache::release_owned_rid()` — the same queue and render-thread discipline as above.
- The armed RID lives in a side table keyed by the texture's instance id, not inside the attached object: `disarm()` must work without `Object::cast_to`, which is ClassDB-based on 4.7 and fails in builds that link these sources without module registration (`gdextest`).
- `store()` disarms on acceptance, so a RID is freed exactly once — by the cache or by the owner, never both.
- The queue self-drains: a drain is scheduled even when every RID was deferred, and a drain that re-queues still-in-flight RIDs re-arms on the next drawn frame (`RenderingServer::request_frame_drawn_callback`; never `call_on_render_thread`, which runs inline on the render thread and would recurse). Consecutive no-progress drains stop after 240 attempts with one warning; the RIDs stay queued for the next release.
- Teardown: `flush_deferred_rid_releases(clean)` runs only after `DSContext::shutdown_ds()`, and only when `was_last_shutdown_clean()` is true. If the backend's drain timed out, hardware DMA may still be live, so the queue is left untouched and the RIDs die with the device — matching the backend's own timeout path, which leaks handles deliberately.
- `DSTextureOwner::shutdown()` runs first in module deinit (late destructors skip the engine) and `initialize()` re-arms it on module init, so an init → uninit → init cycle cannot leave releases latched off.
- The retry cap resets on progress, on an empty drain, **and** whenever a new wave of deferred RIDs is queued — so a capped, permanently-stuck RID cannot suppress retries for later releases. A drain that re-queues because there is no device arms no retry at all (nothing to free, and Godot never brings a `RenderingDevice` up later in the process).
- Environment assumption this now **depends on**, rather than merely benefiting from: Godot's default Safe thread model, where the main thread *is* the render/server thread (Section 2). Three things rely on it — `flush_deferred_rid_releases()` only frees when `is_on_render_thread()` is true at teardown, the retry posts `request_frame_drawn_callback` from the render thread (its callback list is not locked), and callers of `release_owned_rid()` free inline only because the main thread qualifies. Under a threaded render model this degrades safely (the flush bails without touching the queue, retries wait for the next explicit release) but the exit census stays dirty.
- Extension teardown still has a live device: `MODULE_INITIALIZATION_LEVEL_SCENE` deinit runs **before** `RenderingServer::finish()` / `memdelete(rendering_server)` (`main.cpp:907-920`), so a GDExtension can still query the RenderingDevice and free RIDs there. That is why the exit flush works — do not assume the device is already gone.
- `is_resource_in_flight()` is not valid after shutdown: it short-circuits to `false` on `!initialized || shut_down || !active_backend`, so liveness checks must run **before** `shutdown_ds()`. Free first, shut down second — and do both in one callback rather than relying on teardown order.
- A disabled cache is a **residency** decision, not a performance switch: `store()` refuses, so those textures are owned by `DSTextureOwner` and the GPU path still runs (measured with `texture_cache_enabled=false`: 8/8 loads on the GPU path, 0 leaked RIDs). What it does cost is reuse (every load re-reads and re-decodes), engine residency, and **prewarm** — async prewarm refuses up front on both the GPU and CPU paths (`can_own(CACHE_MODE_REUSE)` false), because a prewarmed texture that nothing holds would be released immediately. Disabling also empties the existing entries.

### Upstream: Engine-Side Survivors Still Leak (godotengine/godot#116815, open)
`Texture2DRD` scope-exit never frees the RID even outside any cache: a tool-script repro (`texture_create` + `set_texture_rd_rid`, texture dropped at function end) exits with `WARNING: 1 RID of type "Texture" was leaked` on D3D12 Forward+ (4.5.1/4.6/master, `needs testing`, no fix). Our game-run D3D12 proof shows the same mechanism from the other side — cache fully cleared (`cached_count=0`), scene instance freed, threaded texture released, 10-frame drain, yet exactly 2 RIDs leak with a clean ObjectDB. Those survivors are engine-held (`ResourceCache` residents via `take_over_path`, scene subresources, thread-load results): no owner remains to `free_rid` them, and the wrapper will not do it. Treat small-count exit residuals with empty caches as this upstream behavior, not as evict-path regressions; only a scaling-with-resolve-count census indicates our bug.

### Test-Script Holder Hygiene for the Shutdown Census
Headless test scripts that retain texture `Ref`s in script-level arrays (or locals) until `quit()` produce their own exit noise — `WARNING: N ObjectDB instances were leaked at exit` plus `ERROR: N resources still in use at exit` — which masks the census above. Clear every holder (`array.clear()`, `unref()` locals) after the final RID check and before `_finish()`; a proof script that does so exits with zero ObjectDB/resource warnings, leaving any residual as a genuine signal.

---

## 28. Per-Path I/O Cost Reality: `exists()` Probes and Cold Opens

### The Engine Reality
Two measured costs on Windows (Godot 4.7.2, NVMe, ~100-path microbenches) dominate texture hand-off time far beyond parsing or allocation:
1. **`ResourceLoader.exists()` averages ~85 usec per call.** Each invocation performs a full loader iteration over every registered `ResourceFormatLoader`. A first-draft gate calling it once per path held warm enqueue at ~9 ms for 100 textures—worse than the ~40 usec `.import` parse it was meant to skip.
2. **First-touch `FileAccess::open()` variance spans 67–640 usec per path.** On a cold OS disk cache, merely *opening* 100 files costs 8–87 ms before a single byte is parsed, while the same set served warm costs a fraction of that.

### The Rules
- Never call `ResourceLoader.exists()` per path inside a hot loop. Probing the `.import` file directly with `FileAccess::file_exists()` measured ~7 usec with outcome-identical results and replaced the gate.
- Budget cold-start I/O, not just parsing: 100 textures cost 1 bulk read instead of 100 cold opens when bundled into `textures.dspkg`. In early iterations, a sidecar index (`textures.headers.idx`) was used to cache loose `.dstex` headers; this was deprecated and removed once `DSPkg` consolidated all entry headers into its in-memory directory table loaded upon opening, giving instantaneous header resolution across all bundled textures without separate sidecars. Cache both outcomes (found path *and* miss) so repeat prewarms serve failures with one stat and zero opens.

---

## 29. Autoloads Initialize Under `-s` Custom-`SceneTree` Runs

### The Engine Reality
The common belief is that running `godot -s headless_tests/test_foo.gd` with a script extending `SceneTree` bypasses project autoloads, since the script replaces the main loop. Empirically (Godot 4.7.2, `--headless --path project -s ...`), **autoload singletons still initialize**: both the `DSBatchManager` autoload (`[DirectStorage] Autoload initialized...`) and unrelated autoloads fired `_ready()` and entered the tree before the test script's `_initialize()` ran.

### The Consequence for Tests
A headless streaming test cannot assume it owns the flush/collect pipeline exclusively—the autoload's `_process()` concurrently calls `flush_stream_async()` / `collect_completed_results()` / `poll_completed()` on the same singletons. This is safe *only* because every step of the frame driver is idempotent (busy flush keeps the queue, harvest collects finished batches, progress polls pure state). Test harnesses must be written against concurrent driving, and must still drive the pipeline manually frame-by-frame for determinism rather than assuming quiescence.

### The Rule
Design autoload frame drivers so that double-driving (autoload `_process` plus manual test pumping) is harmless, and never assert exact submit/flush counts that a concurrent driver could perturb—assert bounds (`1 <= submits <= slack`) instead.

---

## 30. DirectStorage Queue Capacity Saturation & Stall at High Multi-Mip Volumes

### The Engine Reality
DirectStorage queues (`IDStorageQueue`) are instantiated with a fixed request capacity configured via `DSTORAGE_QUEUE_DESC.Capacity` (`thirdparty/directstorage/include/dstorage.h:113-122`; min 128 / max 8192 at `:43-50`). The extension defaults to 2000 (`src/core/ds_context.h:42`, wired at `src/platform/win32/ds_win32_backend.cpp:1561-1563`). In production scenes with numerous large textures (such as `tps-demo` with ~250 textures), multi-mip splitting (Section 8) turns 250 textures into 1,240+ independent subresource requests (`ds_win32_backend.cpp:2132-2196`, one `EnqueueRequest` per mip at `:2079`).

### The Pitfall: Unbounded Enqueue Blocks the Submitting Thread at 84%–93%
If an asynchronous preloader or streaming manager dumps all 1,240 requests into `EnqueueRequest` without chunking, the queue saturates. Per the SDK (`dstorage.h:954-957`), `EnqueueRequest` returns `void` and **blocks** until a free entry exists (auto-submitting at half-full) — it does not fail, and fences still signal. The real hazard is the submitting thread blocking while holding the batch lock, which stalls pipeline progress.

In game benchmarks, this manifested as an indefinite loading freeze where the progress bar halted at 84%–93% while CPU and GPU utilization dropped to near zero.

### The Invariant Rule: Batch Chunking
Never dump unbounded request arrays into a DirectStorage queue in a single flush. Stream submission routines (`flush_stream_async()`, `src/core/ds_context.cpp:840-845`) partition pending requests into chunks:
```cpp
// Submit at most min(capacity / 2, 256) requests per batch (floor 64 for small capacities)
uint32_t batch_size = std::min<uint32_t>(m_queue_capacity / 2, 256);
```
Submitting half-capacity chunks respects the SDK half-full auto-submit threshold and leaves headroom for in-flight completions. Note: only the stream path chunks; the sync `flush_batch()` / `Win32Backend::submit_batch()` path (`ds_context.cpp:736-773`, `ds_win32_backend.cpp:2438+`) submits whole slots with `ERR_BUSY` backpressure instead.

---

## 31. `Texture2DRD` Pipeline Incompatibilities & Bifurcated Prewarm in Godot 4.7 Forward+

### The Architectural Reality
[Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html) is an engine wrapper originally intended for raw compute shader output buffers and viewport textures. Wrapping prewarmed DirectStorage textures in `Texture2DRD` without format provisioning and strict pipeline boundaries triggers three distinct, fatal failures across Godot 4.7's rendering architecture:

1. **2D CanvasItem Batch Renderer Failure (`RendererCanvasRenderRD`)**:
   - In Godot 4.7 Forward+, all 2D canvas drawing (crosshairs, HUD, fonts, UI elements) is processed by `RendererCanvasRenderRD` (`servers/rendering/renderer_rd/renderer_canvas_render_rd.cpp:3017-3023`).
   - Batch uniform-set creation fails when a diffuse / normal / specular / sampler RID is null or invalid. CanvasTexture tracking (`:3029-3040`) is opt-in invalidation only.
   - Raw `Texture2DRD` instances hit the failure below when their backing RIDs are not yet valid or bound as CanvasItem textures:
     ```text
     ERROR: Failed to create uniform set for batch.
        at: (servers/rendering/renderer_rd/renderer_canvas_render_rd.cpp:3023)
     ```
2. **D3D12 sRGB Material Views & the RD Share Gate**:
   - Godot's 3D PBR material pipeline requires an sRGB format view for Albedo sampling (`servers/rendering/renderer_rd/storage_rd/texture_storage.cpp:2221-2233`) created via `texture_create_shared`.
   - The RenderingDevice enforces an RD-level gate: the view format must be in `allowed_shared_formats` (`servers/rendering/rendering_device.cpp:1792-1793`; populated at `:1719`, appended by `_texture_ensure_shareable_format` at `:2591-2596`). Without the entry, `texture_create_shared` returns an invalid `RID()`.
   - The D3D12 driver itself always creates typeless resources (`drivers/d3d12/rendering_device_driver_d3d12.cpp:1280`); the failure is the RD-level early-out, not a D3D12 view rejection. Creating a raw `Texture2DRD` via `rd->texture_create()` without routing through `_texture_ensure_shareable_format()` leaves the sRGB view invalid:
     ```text
     ERROR: Texture (binding: 1, index 0) is not a valid texture.
        at: (servers/rendering/rendering_device.cpp:4502)
     ```
3. **Asynchronous Initialization Race & Placeholder Dummy Substitution**:
   - In Godot engine source (`scene/resources/texture_rd.cpp:82`), `Texture2DRD::set_texture_rd_rid()` does **not** immediately bind the texture; it defers assignment to the render thread via `RenderingServer::call_on_render_thread()`.
   - If a scene node or material queries `get_rid()` on the main thread before that callback executes, `texture_rd.cpp:57-60` detects `texture_rid.is_null()` and substitutes a temporary placeholder dummy (`texture_2d_placeholder_create()`).
   - Materials bound against the placeholder can then hit generic draw-list validation errors (invalid-texture at `rendering_device.cpp:4502`, or `Uniforms were never supplied for set (%d)` at `:6033,:6197,:6562,:6845,:6985` which fires when no set is bound at all). The placeholder masks the null but binds the wrong content/format.

### The Rule
Bifurcate prewarm: wrap 3D PBR color textures in `Texture2DRD` with an sRGB shareable format; restrict `ImageTexture` to 2D UI, decals, and CPU readback paths.

### The Solution: Bifurcated Asset Prewarm Policy
Rather than abandoning GPU DirectStorage streaming entirely or applying `Texture2DRD` indiscriminately, Godot 4.7's architecture mandates a bifurcated pipeline:

1. **3D PBR Color Textures (BC1/BC2/BC3/BC7, RGBA8 Family)**:
   - Streamed via DirectStorage GPU GDeflate DMA directly into VRAM and wrapped in [Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html).
   - **Fix for Failure #2 (sRGB share gate)**: Call `rd_fmt->add_shareable_format(base)` followed by `rd_fmt->add_shareable_format(srgb)` before `texture_create()` (see Section 44 / Q44).
   - **Fix for Failure #3 (Async placeholder race)**: Allocate and wrap `Texture2DRD` during the batched prewarm step. When the scene later loads via `ResourceLoader.load(..., CACHE_MODE_REUSE)`, the render-thread RID assignment has already completed and cached textures bind without placeholders.
2. **2D/UI, Decals, Light3D Projectors, and CPU Readback Assets**:
   - Strictly prewarmed/loaded as standard [ImageTexture](https://docs.godotengine.org/en/stable/classes/class_imagetexture.html) (via `DSTEX_FLAG_AS_IMAGE_TEXTURE`).
   - Eliminates Failure #1 (`RendererCanvasRenderRD` uniform set crashes) and completely avoids synchronous GPU staging readback stalls (`texture_get_data()`).

---

## 32. Sub-Thread Worker Contention & Cooperative CPU Decompression Fallback

### The Engine Reality
When running concurrent prewarming and threaded scene loading (Async Prewarm running concurrently with `ResourceLoader.load_threaded_request()`, referred to as Approach B in Section 15), Godot's `WorkerThreadPool` background threads parse scene nodes and resolve texture dependencies while the DirectStorage streaming queue is actively in flight.

### The Pitfall: Worker Thread Blocking
If a background worker thread calling `DSResourceFormatLoader::load()` attempts to enqueue DirectStorage requests while the stream queue is busy, or waits on DirectStorage hardware completion fences, thread contention occurs. If worker threads block on busy DirectStorage slots, the entire scene loading thread stalls.

### The Rule: Cooperative Sub-Thread Diversion
When `p_use_sub_threads` is true and DirectStorage has pending streams (`has_stream_pending()`) or uncompleted batches, the loader must immediately divert to fast multi-threaded CPU software decompression (`GDeflateDecompressor`):
```cpp
bool ds_usable = ds_ctx && ds_ctx->is_initialized();
if (p_use_sub_threads && ds_usable) {
    if (!ds_ctx->is_batch_complete() || ds_ctx->has_stream_pending()) {
        ds_usable = false; // Fast cooperative CPU fallback
    }
}
```
This keeps background worker threads completely non-blocking and prevents thread starvation during concurrent scene parsing.

---

## 33. Concurrent Pipeline In-Flight Dependency Awaiting (`pending_paths` & Condition Variable)

### The Engine Reality
When `ResourceLoader.load_threaded_request()` runs concurrently with `DSPreloader` streaming (the concurrent two-pass pipeline, Section 15 Approach B), a background worker thread may encounter an asset that `DSPreloader` has already submitted to DirectStorage but has not yet finished streaming or storing into `DSTextureCache`. Without synchronization, the worker thread risks performing redundant disk reads or racing on cache insertion.

### The Solution: Path Refcounting & Condition Variable
1. `DSPreloader` tracks all in-flight texture paths under a mutex:
   ```cpp
   std::unordered_map<String, int> pending_paths;
   std::condition_variable pending_cv;
   ```
2. When a worker thread in `DSResourceFormatLoader` needs a texture, it checks `DSPreloader::is_texture_pending(path)`.
3. If pending, the worker thread waits on `pending_cv.wait_for(...)` until the preloader finishes streaming.
4. When `DSPreloader::poll_completed()` stores the ready texture into `DSTextureCache`, it decrements `pending_paths` and fires `pending_cv.notify_all()`. The worker thread awakens and immediately retrieves the pre-warmed texture from cache.

### The Render-Thread Deadlock Guard
If `await_pending_texture()` is called from the **render thread**, it MUST NOT sleep on `std::condition_variable`—because `DSPreloader::poll_completed()` itself requires the render thread! Sleeping would deadlock the engine. On the render thread, `await_pending_texture()` must synchronously pump completions (`collect_completed_results()` + `poll_completed()`) inline.

---

## 34. Exported Build Texture Discovery: `.godot/imported` Absence & Sidecar `.dspkg` Querying

### The Engine Reality
In the editor and development builds, imported textures reside as loose `.dstex` files inside `.godot/imported/`. In exported release or debug builds, `.godot/imported/` does **not** exist on disk, and textures are stripped from `.pck` into loose sidecar container archives (`textures.dspkg`, Section 21) when bundling is enabled in the export plugin.

### The Pitfall: Empty Directory Scans in Exported Binaries
Any prewarming script that relies on scanning directory trees (`res://...`) or probing `.godot/imported` via `FileAccess.file_exists` will find 0 textures in an exported binary, silently failing to prewarm any assets.

### The Rule: Direct `.dspkg` Archive Querying
In exported builds (`OS.has_feature("template")`), prewarming routines must query `DSPkg` directly:
```gdscript
if OS.has_feature("template"):
    var dspkg := DSPkg.new()
    if dspkg.open(ProjectSettings.globalize_path("res://textures.dspkg")) == OK:
        for entry in dspkg.get_entries():
            texture_paths.append(entry)
```
Querying the package manifest directly from the sidecar archive header guarantees 100% texture discovery in exported games without relying on the editor-only imported cache directory.

---

## 35. Texture Cache Capacity Eviction vs. Externally-Bound `Texture2DRD` RIDs

### The Engine Reality
`DSTextureCache` defaults to `max_entries = 256` (`src/integration/ds_texture_cache.h:28`, overridable via the `directstorage/texture_cache_max_entries` project setting or `set_max_entries()`). When a bulk prewarm stores ~250 level textures on top of a handful of boot/menu residents, LRU pressure evicts the oldest entries — typically the menu's own textures. Per the Section 27 discipline the cache then calls `free_rid()` on evicted `Texture2DRD` RIDs, which is correct only while the cache is the sole owner.

### The Pitfall: Evicting Textures That Are Still Drawn
Eviction drops the cache's `Ref`, but materials in the still-visible menu scene (3D background, `TextureRect` art outside hidden containers) keep drawing those exact objects every frame — now with dead RIDs. The result is per-frame spam with zero DS-side warnings (every load succeeded):
```text
uniform_set_create: Texture (binding: 1, index 0) is not a valid texture.
draw_list_draw: Uniforms were never supplied for set (3) ...
```

### The Diagnostic Signature
Attribution is almost mechanical: clean menu idle + spam only while the loading screen is up + clean gameplay + error count scaling with load duration (not asset count) = the cache evicted a texture the visible scene still binds. `DSTextureCache.get_cached_count()` at Play press plus the prewarm set size predicts the collision exactly (e.g. 6 + ~250 against a 256 cap).

### The Rules
1. Size capacity with headroom for residents plus the largest prewarm set (`texture_cache_max_entries=2048` for a ~250-texture project with menu residents).
2. Eviction skips entries still held outside the cache (implemented in `src/integration/ds_texture_cache.cpp`): the cache holds exactly one `Ref` per canonical entry and aliases add none, so `get_reference_count() > 1` means a live outside owner (scene material, `TextureRect`, handed-out `get_cached()` Ref, engine `ResourceCache` resident via `take_over_path`). `_evict_oldest()`, `store()`, and `set_max_entries()` all scan oldest-first past held entries; when every entry is held, the cache keeps temporary over-capacity with a `push_warning` naming the path instead of force-freeing the oldest live RID (the old mass-clear fallbacks are removed). Verified at cap 16 with 133 prewarmed textures over 5 cache-resident menu textures: zero rendering errors, all menu RIDs valid.

---

## 36. RID Namespace Gotcha in GDScript Diagnostics (`texture_is_valid`)

### The Engine Reality
In Godot, `Texture2D.get_rid()` returns a `RenderingServer` RID across all texture subclasses — including both `ImageTexture` (`scene/resources/image_texture.cpp:132-138`) and `Texture2DRD` (`scene/resources/texture_rd.cpp:56-63`).
`Texture2DRD` wraps an underlying `RenderingDevice` texture RID, but exposes it via `get_texture_rd_rid()` (bound as the GDScript property `texture_rd_rid`, `texture_rd.cpp:117-119`).
Meanwhile, `RenderingDevice.texture_is_valid(RID)` (`servers/rendering/rendering_device.cpp:2915-2919`) strictly queries `RenderingDevice`'s internal texture owner (`texture_owner.owns(p_texture)`).

### The Pitfalls
1. **Passing `get_rid()` to `RenderingDevice`**: Passing `tex.get_rid()` to `rd.texture_is_valid()` passes a `RenderingServer` RID into the `RenderingDevice` namespace, returning `false` (or throwing errors) even for valid `Texture2DRD` instances.
2. **False Positives on `ImageTexture`**: A diagnostic script that validates scene textures against `RenderingDevice` cannot check `ImageTexture` at all, because `ImageTexture` device allocations are managed internally by `RenderingServer` and do not expose a `texture_rd_rid`. Checking an `ImageTexture` with `rd.texture_is_valid()` flags every healthy CPU/streamed texture as invalid.

### The Rule
When writing GDScript test harnesses or diagnostics to verify hardware VRAM residency:
- Gate the check on `tex is Texture2DRD`.
- Inspect `(tex as Texture2DRD).texture_rd_rid`, **never** `tex.get_rid()`.
- Guard with `rd != null`, `rd.has_method("texture_is_valid")`, and an early-out for headless/Compatibility runs (Section 11):
```gdscript
if tex is Texture2DRD:
    var rd_rid: RID = (tex as Texture2DRD).texture_rd_rid
    if rd and rd.has_method("texture_is_valid"):
        assert(rd.texture_is_valid(rd_rid), "Texture2DRD has invalid device RID")
```

---

## 37. Prewarm Progress Units Are Stream Requests, Not Textures

### The Engine Reality
A prewarm tag total starts at 0 and bumps once per enqueued stream request (`src/integration/ds_preloader.cpp:920-923`). With multi-mip splitting (Section 8), one texture issues ~5 requests, so a 30-texture chunk reports progress in units of ~150.

### The Pitfall: Count-Based Bar Mapping Overshoots
Treating `done` as a texture count (`(base + done) / total_textures`) overshoots the bar to clamped-full mid-chunk, then snaps back at the next chunk — a visible flash between full and actual. Map the ratio instead, and guard the `(0, 0)` initial callback:
```gdscript
var ratio := float(done) / float(maxi(total, 1))
bar.value = ((base_count + ratio * chunk_count) / total_textures) * max_progress
```

---

## 38. Loading-Screen Errors Belong to the Visible Scene, Not the Loading One

### The Engine Reality
During `ResourceLoader.load_threaded_request()` the loading scene is parsed off-tree and never drawn; the menu scene (3D background, environment, sky, lights, unhidden UI) keeps rendering every frame until the swap.

### The Pitfall: Misattributing Draw Errors to the Load Pipeline
Per-frame rendering errors during loading therefore implicate actors in the *visible* menu — including non-mesh ones a material-only audit misses (`TextureRect` art, `PanoramaSkyMaterial`, light projector slots, volumetric fog). Hiding menu visuals at Play press is both a one-line discriminator (spam vanishes = menu actor) and a UX fix: it also removes the frozen-background frame Section 16 warns about.

---

## 39. Refcount Guards Must Read Pristine Counts (No Retain-Before-Check)

### The Engine Reality
In godot-cpp, every `Ref<T>` copy — including a `push_back` into a holding vector — increments the pointed-to `RefCounted`'s reference count immediately. A guard of the form `get_reference_count() > 1 means externally held` therefore observes not just outside owners but every local copy the checking code itself has already taken.

### The Pitfall: Retain-Before-Check Skips the True Oldest
The first cut of the Section 35 guard kept the legacy push-before-evict pattern: the caller pushed the oldest entry's `Ref` into the evicted list, *then* `_evict_oldest()` scanned counts. The push raised the oldest entry's count to 2, so the guard always classified the true oldest as externally held and silently evicted the second-oldest instead — 4 existing cache tests failed with wrong-victim eviction (never a crash or leak, which is why it looked almost right).

### The Rule
Capture candidate `Ref`s only *after* selection, inside the evictor: `_evict_oldest()` scans pristine counts, and only the selected victim's `Ref` is retained for the render-thread RID release (`src/integration/ds_texture_cache.cpp`, `src/integration/ds_texture_cache.h`). Never snapshot-then-scan; any helper that returns "the evictable entry" must return it by path/key and let the caller retain after removal is decided.

---

## 40. `ResourceLoader.load_threaded_request()` with `use_sub_threads = true` Causes Deadlocks and Race Conditions

### The Engine Reality
In Godot 4.x (`core/io/resource_loader.cpp` and `core/io/resource_format_binary.cpp:639`), setting `p_use_sub_threads = true` instructs Godot's binary format loader to distribute external scene sub-dependencies (`.glb`, `.scn`, `.tres`, `.wav`) as parallel asynchronous tasks across Godot's `WorkerThreadPool` (`LOAD_THREAD_DISTRIBUTE`).

### The Bug / Deadlock Mechanism
When a complex scene contains multiple models or sub-scenes that share common dependencies (e.g. shared materials, collision shapes, sound effects):
1. **Race Condition in `thread_load_tasks`**: Sub-tasks for internal dependencies are registered without user-facing load tokens (`p_for_user = false`). When one worker thread finishes loading a shared sub-resource, it unreferences the task and removes it from `thread_load_tasks`.
2. **Missing Token Assertion Failure**: A second worker thread that was awaiting or loading the same dependency calls `_load_complete_inner()`, finds `!thread_load_tasks.has(local_path)` at `core/io/resource_loader.cpp:1041`, and logs:
   ```text
   ERROR: Bug in ResourceLoader logic, please report.
      at: _load_complete_inner (core/io/resource_loader.cpp:1041)
   ERROR: Can't load dependency: 'res://path/to/material.tres'.
   ```
3. **Deadlock in `WorkerThreadPool::yield()`**: If tasks have mutual or nested dependencies, worker threads can enter an unresolvable wait loop in `_run_load_task()`, hanging permanently at 85%–92% completion. No error is emitted while deadlocked because the threads are suspended waiting on condition variables.
4. **Cascading Teardown Errors on Window Close**: If the user closes the frozen window, Godot begins engine teardown. The suspended worker threads wake up to an already-erased `thread_load_tasks` table, dumping dozens of `Can't load dependency` and `Failed loading resource` errors simultaneously.

### The Rule
Never pass `use_sub_threads = true` to `ResourceLoader.load_threaded_request()` for complex scenes with interdependent hierarchies. Always set `use_sub_threads = false`:
```gdscript
ResourceLoader.load_threaded_request(LEVEL_PATH, "", false, ResourceLoader.CACHE_MODE_REUSE)
```
Because DirectStorage has already prewarmed and cached all heavy textures in `DSTextureCache` and `ResourceCache`, single-threaded background parsing is near-instantaneous (~300 ms), completely stable, and immune to `WorkerThreadPool` race conditions.

---

## 41. Main-Thread Frame Hitches from Fixed-Count Texture Polling & Synchronous Scene Dependency Traversal

### The Engine Reality
When prewarming textures on background NVMe DirectStorage queues, I/O decompression completes asynchronously across worker queues at multiple gigabytes per second. To make these completed payloads visible to Godot, the main engine thread must ingest and wrap them into usable engine texture objects (`ImageTexture` or `Texture2DRD`) via `DSTextureCache.poll_completed()`.

### The Two Frame Hitch Sources
Benchmarking loading screen frame times on real hardware (e.g. NVIDIA GTX 1050 Ti) revealed two distinct causes of main-thread frame freezes:

1. **Frame #2 Dependency Crawl Freeze (~136 ms stall)**:
   - Calling `DSPreloader.collect_scene_textures(scene_path)` synchronously traverses the root scene's dependencies using `ResourceLoader.get_dependencies()`.
   - On exported binary scenes (`.scn`), parsing deep dependency trees, embedded materials, and mesh variants on the main thread takes **100 ms to 150 ms**, freezing the loading screen immediately upon button press before progress starts.

2. **Frames #6–#16 Fixed-Count Ingestion Hitches (43 ms to 112 ms periodic freezes)**:
   - Polling completions with a fixed item count (e.g. `poll_completed(tag, 8)`) assumes uniform texture creation cost.
   - On desktop GPUs, creating an unpadded 2048x2048 or 4096x4096 BC1/BC3 texture (including `unpad_footprint_mip` row unpadding and `ImageTexture::create_from_image`) costs **8 ms to 14 ms** per texture.
   - Processing 8 large textures in a single frame consumes **64 ms to 112 ms** of synchronous CPU execution time on the main thread, resulting in visible 4-to-7 frame video freezes during progress bar movement.

### The Fix: Microsecond Time Budgeting & Exported Manifest Fast-Path
1. **Microsecond Time Budgeting (`p_max_usec`)**:
   Instead of a fixed texture count, provide a microsecond time limit to `poll_completed(tag, max_items, max_usec)`. By measuring `Time::get_singleton()->get_ticks_usec()` after each texture build and yielding when `elapsed >= p_max_usec` (e.g. `4000` usec / 4 ms):
   - Large 4K textures yield after just 1 texture (~8 ms elapsed), leaving ample headroom for 60 FPS rendering.
   - Small 256x256 textures batch multiple items within the same 4 ms budget.
   - Main-thread frame time never exceeds 16.6 ms, completely eliminating frame stutters.
   - An entry that does not fit the remaining budget is no longer built inline at all: CPU builds are handed to `WorkerThreadPool` (measured 20-27 ms for a 4096x4096 RGBA8 payload, so a 2-4 ms slice cannot contain one). The first item is no longer exempt from the budget; only constant-cost GPU entries are.

2. **Instant Package Manifest Lookup (<0.5 ms)**:
   In exported builds where `textures.dspkg` is deployed, the archive already contains the pre-indexed list of all bundled textures. Querying `dspkg.get_entries()` directly against the requested coverage directories discovers all dependencies in **<0.5 ms**, completely bypassing the 100+ ms binary scene crawl.

---

## 42. Discarding Speculative Threaded Loads (`load_threaded_get` Consumption)

### The Engine Reality
When loading resources on worker threads via `ResourceLoader.load_threaded_request()`, Godot instantiates an internal `ThreadLoadTask` in `core/io/resource_loader.h`. This structure holds a strong `Ref<Resource> resource` and a `LocalVector<Ref<Resource>> resource_dependencies` container to ensure all sub-resources remain allocated during background parsing. Godot provides no cancellation method (such as `cancel_threaded_request()`). The task and its strong references remain registered in Godot's internal tracking tables until `ResourceLoader.load_threaded_get()` is called, which consumes the task and unregisters it.

### The Trap / Symptom
If a game issues a speculative `load_threaded_request()` during the main menu or a lobby (e.g. preloading `level.tscn` in anticipation of a Play click), and the player toggles off the option or leaves the menu without launching the scene, the un-retrieved task keeps the entire parsed scene graph alive in memory indefinitely.
In a production 3D title, this locks substantial GPU VRAM:
* High-dynamic-range `LightmapGI` baked texture arrays (e.g., `level.exr` uncompressed in VRAM): **~280 MiB**.
* 3D GLB mesh vertex, normal, tangent, and index buffers: **~180 MiB**.
* Total wasted VRAM: **~460 MiB** retained in memory even though the level is never played.

### The Rule
To cancel or trash a background `ResourceLoader.load_threaded_request()`, wait for completion and call `load_threaded_get()` then immediately nullify the reference; Godot has no cancel API and retains strong references permanently until consumed.

```gdscript
func _discard_speculative_scene_async(path: String) -> void:
	var status := ResourceLoader.load_threaded_get_status(path)
	while status == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
		await get_tree().process_frame
		status = ResourceLoader.load_threaded_get_status(path)
	if status == ResourceLoader.THREAD_LOAD_LOADED:
		var discarded: Resource = ResourceLoader.load_threaded_get(path)
		discarded = null # Reference count drops to 0; Godot frees lightmaps and mesh buffers from VRAM
```
*(Implemented in `tps-demo/menu/menu.gd:334`)*

---

## 43. Unconditional Cache `clear()` and External RID Retention

### The Engine Reality
In GDExtensions managing custom texture resources (`Texture2DRD` or native `RenderingDevice` texture allocations), the extension owns the underlying GPU texture RIDs. If an extension passes a texture RID to `RenderingDevice::free_rid()` while Godot's `StandardMaterial3D` or canvas pipelines hold active shader uniform sets (Set 3) binding that RID, Godot's rendering pipeline breaks immediately.

### The Trap / Symptom
If a texture's GPU RID is destroyed while an active scene references it, Godot attempts to build or bind the uniform set on the next frame and fails continuously:
```text
[1] uniform_set_create: Texture (binding: 1, index 0) is not a valid texture. Parameter "texture" is null.
    -> <C++ Source> servers/rendering/rendering_device.cpp:4502 @ uniform_set_create()
[2] draw_list_draw: Uniforms were never supplied for set (3) at the time of drawing, which are required by the pipeline.
    -> <C++ Source> servers/rendering/rendering_device.cpp:6033 @ draw_list_draw()
```
The visible mesh turns completely black (drawing only untextured emission or fallback shaders), flooding the debugger with thousands of error messages per second.

Furthermore, calling unconditional `DSTextureCache.clear()` during active gameplay wipes all entries from `cache_map`, `alias_map`, and the LRU list. Even though external reference guards protect the GPU RIDs from being destroyed, the cache loses track of active textures, destroying deduplication and causing subsequent requests to perform redundant disk reads and memory allocations.

### The Rule
Use `clear_unused()` during active gameplay to evict only unreferenced textures while preserving cache hits for live scenes; reserve `clear()` for scene transitions or teardown to avoid wiping cache entries for in-use assets.

To guarantee GPU safety, both `DSTextureCache::clear()` and `DSTextureCache::clear_unused()` gate texture RID release behind `_is_entry_evictable_locked()` and `ds_texture_cache_is_externally_held()`:
```cpp
static bool ds_texture_cache_is_externally_held(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null() || !p_texture.is_valid()) {
		return false;
	}
	return p_texture->get_reference_count() > 1; // Held by active scene material or node outside cache
}
```
However, their cache retention contracts differ:
- `DSTextureCache::clear_unused()` scans canonical entries and removes **only** unreferenced textures (`ref_count == 1`). Textures currently bound to active nodes or materials remain in `cache_map`, keeping deduplication and subsequent `get_cached()` lookups instant.
- `DSTextureCache::clear()` preserves externally held GPU RIDs from destruction, but empties `cache_map` completely (`cache_map.clear()`).

In GDScript, call `clear_unused()` whenever toggling background preloads off or releasing idle level caches during active gameplay:
```gdscript
if ClassDB.class_exists("DSTextureCache"):
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if cache != null and cache.has_method("clear_unused"):
		var freed_count: int = cache.clear_unused()
		print("[Cache] Safely evicted %d unreferenced textures" % freed_count)
```
Reserve `clear()` strictly for module teardown (`~DSTextureCache()`) or scene transitions where all previous scene nodes and materials have been completely freed.

## 44. Shareable Format Lists Must Contain the Base Format

### The Engine Reality
`RenderingDevice::texture_create()` validates the format's shareable list up front (`servers/rendering/rendering_device.cpp:1552`): when the list is non-empty, the texture's own base format must be present in it, otherwise creation fails closed with a null RID. The sRGB counterpart alone is not sufficient, even though only the counterpart view is what shaders actually sample.

### The Trap / Symptom
Adding only the sRGB counterpart via `add_shareable_format(srgb)` (the intuitive recipe for 3D PBR albedo sampling) fails every allocation:
```text
ERROR: If supplied a list of shareable formats, the current format must be present in the list
    at: texture_create (servers/rendering/rendering_device.cpp:1553)
```
The null RID silently kills the GPU branch, which then falls through to the CPU path — where a second, unrelated failure can mask the root cause entirely. This cost one red build plus a full debug cycle (see Q02 amendment).

### The Rule
Always `add_shareable_format()` the base format itself first, then the sRGB counterpart.
```cpp
rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(p_pixel_format));
rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(srgb_fmt));
```
Skip the list entirely when the format is already sRGB or has no D3D12 sRGB counterpart (floats, BC4/BC5/BC6, R16 family, and single/dual-channel masks R8, RG8, R8G8B8 — see Q31).

**D3D12 Driver Shareable Validation Trap**: In Godot's D3D12 driver (`rendering_device_driver_d3d12.cpp:1296`), every format in `shareable_formats` is checked against `RD_TO_D3D12_FORMAT[curr_format].family == DXGI_FORMAT_UNKNOWN`. DXGI does not support sRGB for 1- or 2-channel formats (`R8_SRGB` and `R8G8_SRGB` map to `{}` / unknown). If an invalid counterpart like `R8G8_SRGB` is added to `shareable_formats`, the driver fails with:
```text
ERROR: Format 'R8G8_Unorm' is not supported.
ERROR: servers/rendering/rendering_device.cpp:1704 - Condition "!texture.driver_id" is true. Returning: RID()
```
Notice the error prints the *base format* name (`FORMAT_NAMES[p_format.format]`), masking that the failure was caused by the illegal shareable counterpart. Handled in `src/formats/dstex_format.cpp:378` and `src/integration/ds_resource_format_loader.cpp:323`.

## 45. CPU Fallback Rejects Partial Mip Chains

### The Engine Reality
`Image::create_from_data()` with `use_mipmaps=true` demands the complete Godot mip chain down to 1x1 (`core/io/image.cpp:2455`). A 64x4 RGBA8 texture needs 7 levels totaling 1404 bytes. GPU upload has no such requirement — DirectStorage happily decompresses whatever subresources the file carries.

### The Trap / Symptom
A test asset carrying only 2 mips (1280 bytes) loads fine on the GPU path but fails the CPU fallback with:
```text
ERROR: Expected Image data size of 64x4x4 (RGBA8 with 6 mipmaps) = 1404 bytes, got 1280 bytes instead.
    at: initialize_data (core/io/image.cpp:2455)
ERROR: DSResourceFormatLoader: Failed to create GPU/Image texture for user://test_thread_affinity_texture.dstex
```
This asymmetry means a green GPU-path test can hide a broken CPU fallback; the sub-thread test (which forces CPU) caught it. Partial-chain production files would hit the same wall (tracked as issue #0039).

### The Rule
Never pass `use_mipmaps=true` to `Image::create_from_data` with partial mip chains; slice mip 0 and load with `use_mipmaps=false`.
Implemented via Graceful Base Fallback in `src/integration/ds_resource_format_loader.cpp:505,576` and `src/integration/ds_preloader.cpp:1092`: `ds_full_mip_chain_count(w, h)` detects partial chains (`1 < mips < full_chain`), copies base mip 0 pixels, and creates the `Image` with `use_mipmaps=false` alongside a warning (resolving issue #0039). Test payloads also build every level to 1x1 (64x4 = 7 mips) in `project/headless_tests/test_thread_affinity.gd` so mipmaps are preserved.

## 46. Engine Posts Throw Under Our Mutex

### The Engine Reality
`RenderingServer::call_on_render_thread()` is safe to call from extension threads (the Win32 backend's destination-query rendezvous depends on it), but invoking it while holding an extension-owned `std::mutex` threw an unknown (non-`std::exception`) object that escaped synchronously on the calling thread — no engine log, no crash, JSON still written. Bisected with stderr markers: identical post lock-free passes (`render_post_smoke` + `dma_fills_vram_texture` green on D3D12).

### The Trap / Symptom
`prewarm_gpu_async threw an unknown exception` from inside `pump_gpu_prewarm`, between the `Callable` construction print and the posted print — every statement individually innocent, only the under-lock post threw.

### The Rule
Never call into the engine while holding extension mutexes: pop state under lock, release, then post.
```cpp
// pump_gpu_prewarm: pop chunk under state().mutex, release, then post.
RenderingServer *rs = RenderingServer::get_singleton();
if (rs != nullptr) {
    Callable gpu_chunk_callable = callable_mp_static(process_gpu_chunk_on_render_thread);
    rs->call_on_render_thread(gpu_chunk_callable);
}
```
See Q02 (thread affinity), Q33 (render pump).

## 47. Fixture Deletes on Fresh res:// Files Fail (Defender Locks and the DirectStorage File Cache)

### The Engine Reality
Godot's safe-save writes to a temp file and renames over the target at `close()` (`drivers/windows/file_access_windows.cpp:277`). In editor test runs, antivirus scan windows on brand-new `res://` files fail the rename, and `DirAccess::remove` on the same locked files fails too — both silently if return codes are ignored. A stale `textures.dspkg` then survives deletes and overwrites and poisons every later resolve in the process.

A second, deterministic lock source is ours: the Win32 backend keeps every staged archive open in its `IDStorageFile` cache for the life of the context, so Windows refuses the delete even with a correct absolute path and no antivirus involved. `DSContext::shutdown_ds()` releases those handles; nothing else does.

### The Trap / Symptom
```
ERROR: Safe save failed. ... running a paranoid antivirus ...
    at: _close (drivers/windows/file_access_windows.cpp:277)
```
Followed by `has_texture()` false on a just-staged archive whose staging reported success, and a stale entry dump (`mem`/`ctl` paths where `dma` paths were staged).

The same leftover appears with no antivirus at all when the archive was streamed through the Win32 backend: the delete is refused while `DSContext` is alive, a cleanup that ignores the return code reports success, and the *next* run dies on the stale file:

```
ERROR: DSPkg: Could not open file for writing: .../project/textures.dspkg
```

### The Rule
In editor test runs, treat every `res://` write and delete as suspect: remove + copy + reopen-verify + one 500 ms retry, never trust the copy return code alone, make removal failures loud (`push_warning`), and shut DirectStorage down before deleting staged archives because its `IDStorageFile` cache keeps them open. Implemented in `tests/gpu_prewarm_tests.cpp` (`stage_gpu_dspkg`, `remove_abs`); the shutdown-before-delete ordering is in `project/headless_tests/test_prewarm_and_threading.gd`, `test_dspkg_alignment.gd` and `test_integration.gd` (`_shutdown_ds()` before the remove loop).

## 48. Background Ingestion Tasks Must Not Starve Main-Thread GPU Texture Completion

### The Engine Reality
In Godot 4.x, [Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html) wrappers around GPU texture RIDs cannot be instantiated on off-thread worker pools; they require main or render thread affinity (Q02 & Q25). In C++, `DSPreloader::poll_completed()` explicitly gates GPU textures with `can_build_gpu = main_or_render_thread()`. Background worker tasks skip all GPU items, leaving them to be completed by the main thread.

### The Trap / Symptom
In `DSBatchManager._process()`, delegating ingestion to `WorkerThreadPool` whenever `cache.get_unbuilt_prewarm_count() > 0` while guarding the main-thread poll behind `if _poll_task_id == -1` causes a permanent deadlock during GPU prewarm:
1. `get_unbuilt_prewarm_count() > 0` spawns a background worker slice.
2. The active worker task makes `_poll_task_id != -1`, starving the main thread from running `poll_completed()`.
3. The worker task runs `poll_completed()` off-thread, skips all GPU textures, and finishes with 0 items built.
4. Next frame, the task is reaped, but the unbuilt count is still non-zero, immediately re-spawning the worker task before the main-thread poll can execute.
5. In game loading screens (e.g. `tps-demo`), loading hangs indefinitely at 60% (prewarm completion phase) with 0 errors.

### The Rule
Always drain completed GPU textures directly on the main thread in `DSBatchManager._process()` without gating on worker task idle state. Only spawn `WorkerThreadPool` ingestion tasks when unbuilt CPU textures actually exist (`get_unbuilt_cpu_prewarm_count() > 0`), and prioritize completed GPU entries first in `poll_completed()`. Handled in `ds_batch_manager.gd:166-204`, `ds_preloader.cpp:1640`, and `ds_texture_cache.cpp:212`.

## 49. Internal ProjectSettings & 64-Bit Integers Cause Inspector Warnings

### The Engine Reality
Godot 4's Inspector UI for integer properties (`EditorPropertyInteger` in `editor/inspector/editor_properties.cpp`) uses `EditorSpinSlider`, which stores and edits numerical values internally using IEEE-754 64-bit `double` floats. The maximum integer that can be represented with exact precision in a 64-bit float is `2^53 - 1` (`9,007,199,254,740,991`). When an integer setting exceeds this range, Godot warns about float truncation. Furthermore, calling `ProjectSettings.set_setting()` registers properties as user-visible in the Project Settings dialog by default.

### The Trap / Symptom
```
WARNING: editor/inspector/editor_properties.cpp:1611 - Cannot reliably represent '282234329283682123' in the inspector, value is too large.
```
When an addon or export plugin injects 64-bit hashes, bitmasks, or handoff flags (such as `(guid << 32) | manifest` archive pairing fingerprints or `bundle_dspkg` export booleans) into `ProjectSettings`, opening **Project -> Project Settings...** emits this warning spam. In addition, internal exporter state pollutes the settings tree and confuses developers about whether settings should be configured in Project Settings or in Export Presets.

### The Rule
Always call `ProjectSettings.set_as_internal(name, true)` for runtime handoff keys, export artifacts, and 64-bit integer values; Godot's Inspector represents integers with `double` floats and warns when values exceed 2^53 - 1 (`9,007,199,254,740,991`). Handled in `ds_export_plugin.gd:126-130` and `ds_plugin.gd:32-38`.

## 50. Never Include GDExtension DLL Folders in export_presets.cfg include_filter (The .pck Trapping Pitfall)

### The Engine Reality
When Godot exports desktop PC builds (Windows/Linux), its platform exporter (`EditorExportPlatformPC`) is architected to strip folder hierarchies and dump all GDExtension shared libraries (`.dll`, `.so`) directly into the export root directory alongside the `<game>.exe` executable.

At runtime, Godot loads dynamic libraries in `OS_Windows::open_dynamic_library` (`platform/windows/os_windows.cpp`):
```cpp
String path = p_path;
if (!FileAccess::exists(path)) {
    // This code exists so GDExtension can load .dll files from within the executable path.
    path = get_executable_path().get_base_dir().path_join(p_path.get_file());
}
p_library_handle = (void *)LoadLibraryExW(...);
```
If `FileAccess::exists(path)` is false, Godot automatically extracts `p_path.get_file()` and resolves the binary directly in `<exe_dir>`.

### The Trap / Symptom
```
ERROR: Can't open dynamic library: addons/my_plugin/bin/my_plugin.windows.template_debug.x86_64.dll. Error: Error 126: The specified module could not be found.
   at: open_dynamic_library (platform/windows/os_windows.cpp:544)
ERROR: Can't open GDExtension dynamic library: 'res://addons/my_plugin/bin/my_plugin.gdextension'.
   at: open_library (core/extension/gdextension.cpp:811)
ERROR: Error loading extension: 'res://addons/my_plugin/bin/my_plugin.gdextension'.
   at: load_extensions (core/extension/gdextension_manager.cpp:333)
```
When developers add `addons/my_plugin/*` to `include_filter` in `export_presets.cfg`, Godot copies the native `.dll` files **into the virtual `.pck` archive**.
At startup, `FileAccess::exists("res://addons/my_plugin/bin/my_plugin.dll")` returns **true** because the file exists inside the virtual `.pck`.
Because it returns true, Godot **skips the root directory fallback**. It passes the unmapped relative path `addons/my_plugin/bin/my_plugin.dll` to Windows' `LoadLibraryExW`.
Because Windows cannot read DLLs from inside a `.pck` archive and the physical subfolder does not exist on disk, Windows fails with **Error 126**.

In addition:
1. **DLL Naming is Arbitrary:** Godot does not require `windows.template_debug.x86_64` file naming. Any clean name (`dstorage_ext.dll`, `dstorage_ext.debug.dll`) works as long as mapped under `[libraries]` in the `.gdextension` file.
2. **Secondary Dependencies:** External SDK DLLs (e.g., Microsoft's `dstorage.dll` and `dstoragecore.dll`) must reside on the export root next to `<game>.exe` or be registered in the `.gdextension` `[dependencies]` section.
3. **Editor Memory Locking:** Commenting out or modifying an `@tool` `EditorExportPlugin` while the editor is running does not remove the active instance from memory; active editor sessions also lock DLL binaries on Windows.

### The Rule
Never put `.dll` files or GDExtension binary directories in `include_filter` in `export_presets.cfg`. Rely on Godot's built-in GDExtension exporter to place libraries flat in the executable root folder, and avoid custom export plugins attempting to recreate subfolder hierarchies.

---

## 51. GDExtension Editor Binary Redundancy & The 2-Binary Model

### The Engine Reality
Godot developers often assume that because Godot provides an `editor` compilation target (`scons target=editor`) and a `windows.editor.<arch>` key in `.gdextension`, an extension must compile and ship a dedicated editor binary.

In reality:
1. **The Godot Editor is a Debug Build:** In `core/os/os.cpp:481`, Godot defines:
   ```cpp
   #ifdef DEBUG_ENABLED
   if (p_feature == "debug") {
       return true;
   }
   #endif
   ```
   Because the Godot Editor is compiled with `DEBUG_ENABLED`, `OS::has_feature("debug")` evaluates to `true` inside the editor. When resolving `.gdextension` library tags, any entry mapped to `windows.debug.<arch>` or `windows.editor.<arch>` is fully valid.
2. **Compiler Parity in `godot-cpp`:** In `godot-cpp/tools/godotcpp.py`, both `editor` and `template_debug` share the same optimization level (`speed_trace`), debug symbol generation, and debug assertion macros (`DEBUG_ENABLED`).
3. **Editor ClassDB Invariant:** Classes from Godot's `editor/` module (such as `EditorPlugin`, `EditorExportPlugin`, and `EditorImportPlugin`) are compiled with `TOOLS_ENABLED` and **do not exist in `ClassDB` in runtime export templates** (`template_debug` and `template_release`). If a C++ extension defines `EditorPlugin` subclasses, it cannot be loaded in exported games without throwing `Parent class 'EditorPlugin' does not exist!`.

### The Trap / Anti-Pattern
Trying to maintain three separate binaries (`editor`, `template_debug`, and `template_release`) doubles compilation times, complicates remote build pipelines, and requires fragile DLL-aliasing steps to duplicate binaries across editor and debug targets.

Conversely, attempting to rewrite editor-specific tooling (like export plugins and asset importers) into C++ breaks the clean 2-binary workflow by forcing a hard dependency on editor-only classes that are absent at runtime.

### The Rule
When an extension's editor tools (plugins, importers, exporters) are implemented in GDScript, never compile a separate `target=editor` binary; map `windows.editor` to the debug template binary (`*.debug.dll`), as Godot Editor inherently satisfies the `debug` feature tag and shares identical ABI, debug macros, and embedded doc data.

In `directstorage_accelerator.gdextension`:
```ini
[libraries]
windows.editor.x86_64 = "bin/dsgd_64.debug.dll"
windows.debug.x86_64 = "bin/dsgd_64.debug.dll"
windows.release.x86_64 = "bin/dsgd_64.dll"
```

---

## 52. copy_absolute Globalizes Logical Paths, remove_absolute Does Not

### The Engine Reality
Godot's class reference says the same thing about both helpers — "Supports only absolute paths" — but the implementation disagrees (`core/io/dir_access.cpp`):
```cpp
Error DirAccess::copy_absolute(const String &p_from, const String &p_to, int p_chmod_flags) {
	Ref<DirAccess> d = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	// Support copying from res:// to user:// etc.
	String from = ProjectSettings::get_singleton()->globalize_path(p_from);
	String to = ProjectSettings::get_singleton()->globalize_path(p_to);
	return d->copy(from, to, p_chmod_flags);
}

Error DirAccess::remove_absolute(const String &p_path) {
	Ref<DirAccess> d = DirAccess::create_for_path(p_path);
	return d->remove(p_path);          // raw string, no globalize_path
}
```
`rename_absolute` globalizes too. Only `remove_absolute` (and `move_absolute`, same shape) hands the literal `user://...` / `res://...` string to the platform filesystem, where it cannot resolve.

### The Trap / Symptom
A fixture-cleaning block copied from a working staging block looks correct and does nothing:
```
[PROBE] logical paths    err=0 (OK) dst_exists=true     # copy_absolute("user://a", "res://b")
[PROBE] globalized paths err=0 (OK) dst_exists=true
```
`remove_absolute` returns a failure that fixture cleanup usually ignores, so fixtures accumulate silently. When the leftover is the staged archive, the *next* run dies with `DSPkg: Could not open file for writing` (or, on D3D12, keeps the archive locked in the backend's `IDStorageFile` cache until the context is shut down — Q47). Five headless suites carried this pattern before being fixed in one pass.

### The Rule
Always pass `ProjectSettings.globalize_path()` to `DirAccess.remove_absolute()`; never rely on the `user://` or `res://` form, and check the returned Error.
```gdscript
DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
```
Pair it with a loud failure (`_fail`/`push_warning`) rather than a bare call, so a survived fixture is visible (Q47). Fixed in `test_corruption_fuzz.gd`, `test_dspkg_alignment.gd`, `test_integration.gd`, `test_formats.gd`, and `test_prewarm_and_threading.gd`.

---

## 53. `--check-only --script` Cannot Validate Extension GDScript

### The Engine Reality
`godot --headless --check-only --script some.gd` parses the single file in isolation. It does not scan the project, so `preload("uid://...")` cannot be resolved, and it does not initialize GDExtension modules, so classes registered by the extension do not exist in `ClassDB`.

### The Trap / Symptom
A perfectly valid test file reports a wall of errors, and the workspace rule "zero Parse Errors before concluding a turn" looks violated:
```
SCRIPT ERROR: Parse Error: Preload file "uid://ctg5hksd01gao" does not exist.
SCRIPT ERROR: Parse Error: Could not find type "DSRequest" in the current scope.
SCRIPT ERROR: Parse Error: Identifier "DSRequest" not declared in the current scope.
ERROR: Failed to load script "res://project/.../benchmark_common.gd" with error "Parse error".
```
Nothing about the file is wrong. Worse, the check is *silently permissive* about what actually breaks at runtime: a misspelled method, a wrong arity, or a property that does not exist on the extension class all pass, because the parser never learns the class members.

### The Rule
Never treat `godot --check-only --script x.gd` as evidence that a GDScript file using GDExtension classes is valid; verify calls against the C++ bindings or run the suite.
```powershell
# lexical only — expect and ignore the uid/native-class noise
run_headless_test.ps1 -Target project/headless_tests/test_formats.gd
# binding check for anything the parser cannot see
git grep -n 'D_METHOD("enqueue_request"' -- src/
# behaviour: real run with the project path, not check-only
godot_v4_7_2-stable_win64_console.exe --path project --rendering-driver d3d12 `
    --script res://headless_tests/test_dspkg_alignment.gd
```
The concrete case: `ctx.enqueue_request(req, true)` type-checks only because the binding declares `DEFVAL(false)` (`ds_context.cpp:1717`); the parse check would accept the call either way. See Q09 for the D3D12 run mode.

---

## 54. One `await process_frame` May Not Advance `get_process_frames()`

### The Engine Reality
`SceneTree::process()` emits the `process_frame` signal, and the engine's iteration counter (what `Engine.get_process_frames()` returns) is incremented afterwards in `Main::iteration()`. A coroutine resumed by that signal therefore reads the counter *before* the current iteration is counted.

### The Trap / Symptom
In a `SceneTree`-derived test script, `_initialize()` runs before the main loop iterates, so the first resume lands with the counter still at zero:
```
measured: before=0, after one await=0, after two awaits=1
```
An assertion written the obvious way becomes a coin flip — `frame_after > frame_before` fails on the first yield even though a frame was processed and the UI did render. This surfaced as an unreliable "Frame advanced during yield (UI rendered smoothly)" check in the prewarm suite.

### The Rule
In a SceneTree script's `_initialize()`, await `process_frame` twice before asserting `Engine.get_process_frames()` increased.
```gdscript
var frame_before := Engine.get_process_frames()
await process_frame
await process_frame   # first boundary: counter only advances after the second resume
var frame_after := Engine.get_process_frames()
_check(frame_after > frame_before, "Frame advanced during yield (UI rendered smoothly)")
```
Single-await loops that run after the first boundary are fine — they already cross one iteration. `test_prewarm_and_threading.gd` Phase 3.

---

## 55. `has_feature("editor")` Is True for Every CLI Run of an Editor Binary

### The Engine Reality
`OS.has_feature("editor")` reports the *build* of the running binary, not how it was started. Any invocation of the editor executable — including `godot --headless --script res://...`, `--check-only`, and CI runs on a workstation — returns `true`. Only an export template returns `false`.

### The Trap / Symptom
A bypass written for the editor-authoring case silently applies to the entire test suite:
```cpp
if (!force_resolution && ((engine && engine->is_editor_hint()) || (os && os->has_feature("editor")))) {
    return String();   // loader skips res://textures.dspkg
}
```
Because the archive is skipped, the failure appears downstream and looks unrelated to the flag: `test_async_streaming`'s `.dspkg` phase reported `progress (0, 3)` and "batching: 3 textures in 0 submit(s)", and `test_integration` failed loading `res://packaged_test_texture.png` with no error near the cause. Nothing logs where the empty path came from, so the suite looks broken rather than bypassed. `DSPreloader::resolve_archive_path()` has the same check, so both the loader and the prewarm path are affected.

### The Rule
Never gate a shipping-path bypass on `OS.has_feature("editor")` without an explicit opt-in override, because every command-line run of an editor binary reports the editor feature.
```gdscript
const FORCE_ARCHIVE_SETTING := "directstorage/force_archive_resolution"

func _force_archive_resolution() -> void:
	_force_archive_present = ProjectSettings.has_setting(FORCE_ARCHIVE_SETTING)
	_force_archive_prev = ProjectSettings.get_setting(FORCE_ARCHIVE_SETTING) if _force_archive_present else null
	ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, true)

func _restore_archive_resolution() -> void:
	if _force_archive_present:
		ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, _force_archive_prev)
	else:
		ProjectSettings.clear(FORCE_ARCHIVE_SETTING)   # unregistered key: clear, do not write false (Q49)
```
The override is read per call (`ds_resource_format_loader.cpp:153`, `ds_preloader.cpp:69`), so it takes effect immediately; C++ precedent in `tests/gpu_prewarm_tests.cpp`, and the four archive suites now do the same. The rule for the project side is documented in `.agents/rules/gdextest.md`.

## 56. `--script` Runs Only Load Extensions Listed in `.godot/extension_list.cfg`

### The Engine Reality
`GDExtensionManager` loads the `.gdextension` paths named in `res://.godot/extension_list.cfg`. That file is written by the editor's filesystem scan, not by a `--script` run, which starts a `MainLoop` without rescanning `res://`. A `.gdextension` copied into a project after the last scan is present on disk and invisible to `ClassDB`.

### The Trap / Symptom
Copying `addons/directstorage_accelerator/` into a scratch project and running `--headless --script make_fixtures.gd` produced:

```
[fixture] NO_DSTEX
ERROR: Cannot get class 'DSTex'.
```

`.godot/extension_list.cfg` existed but listed nothing. One `--headless --import --quit` wrote the addon path; the unchanged script then created both `.dstex` files.

### The Rule
Always run one `--headless --import --quit` after adding or copying a GDExtension before any `--script` probe, because `--script` runs only load extensions listed in `.godot/extension_list.cfg`.

In practice:

```
godot_console --headless --path <project> --import --quit
```

Then assert `.godot/extension_list.cfg` contains the `.gdextension`. The probe runner does this before every export (`run_probe.ps1`, import step). Related: Q53, Q55.

## 57. An Encrypted PCK Export With No Key Writes a Truncated Pack and Exits 0

### The Engine Reality
`EditorExportPlatform` builds the pack wherever the preset's key is needed; with `encrypt_pck=true` and no key configured it writes a header-only pack and reports success. `EditorExportPlugin` exposes no abort, cancel or fail method and `_export_begin` returns void, so a plugin can refuse to inject bytes but cannot make the export fail.

### The Trap / Symptom
Measured on 4.7.2: missing key in `.godot/export_credentials.cfg`, `encrypt_pck=true` → pack of 112 bytes, exit status 0, no error. A release pipeline that trusts the exit code ships an empty pack.

### The Rule
Never treat an export command's exit status as evidence that an encrypted pack is usable; assert the artifact's size and expected entries afterwards.

Concretely: size floor, expected entries present, key entry encrypted. The plugin's gate only prevents deployment (`ds_export_plugin.gd:346`), so the release workflow must own the failure. Related: Q58.

## 58. A Failed Encrypted-PCK Read Returns `true` and Garbage Bytes

### The Engine Reality
`FileAccessPack` opens the entry and `FileAccessEncrypted` decrypts and verifies an MD5 of the plaintext. With the wrong key it logs and returns an error, but `FileAccess.get_file_as_bytes()` still hands back the buffer it read: correct length, undecrypted content. `FileAccess.file_exists()` consults the pack directory, which is plaintext when `encrypt_directory=false`, so it also says true.

### The Trap / Symptom
Mounting an encrypted pack in a binary without the key (probe stage S4b):

```
[readback] key_exists=true
[readback] key_size=32
ERROR: The MD5 sum of the decrypted file does not match the expected value.
ERROR: Can't open encrypted pack-referenced file "res://.dspkg/toc.key" ... "File corrupt".
```

The 32 bytes were unrelated data, not the key. A size check installs garbage and the next open reports `ERR_UNAUTHORIZED`.

### The Rule
Never validate a PCK-encrypted file by `file_exists()` or byte length; compare the bytes or treat the stderr `File corrupt` line as the only failure signal.

For the key file: compare the candidate against the archive `key_id` (or a known key) and discard on mismatch. Related: Q57.

## 59. The Console Executable Is a Shim; Pipe Redirection Makes It Hang

### The Engine Reality
`Godot_v4.7.2-stable_win64_console.exe` is ~198 KB against the 180 MB engine binary: it launches the real executable with a console attached. Spawned through `System.Diagnostics.ProcessStartInfo` with `RedirectStandardOutput = $true`, the process never exits, even for `--version`.

### The Trap / Symptom
`WaitForExit(20000)` returned false three times for `--version`; the same binary through `Start-Process -RedirectStandardOutput <file>` exits immediately and writes `4.7.2.stable.official.ed1daf0bf`. Cost: two dead-end debugging rounds during the probe build.

### The Rule
Always launch the Godot `*console*.exe` with file-based redirection, never `ProcessStartInfo` pipe redirection, or the shim will not exit.

In practice:

```
Start-Process -FilePath <console.exe> -ArgumentList <args> `
  -RedirectStandardOutput <out> -RedirectStandardError <err> -NoNewWindow -PassThru
```

Implemented in `run_probe.ps1` `Invoke-GodotProcess`. Applies to any script that shells out to Godot.

## 60. `_export_file` Is Not an Inventory When Another Plugin Calls `skip()`

### The Engine Reality
Godot calls every registered `EditorExportPlugin._export_file()` for a file, and `skip()` marks it as handled. In this project the addon calls `skip()` on packaged textures (`ds_export_plugin.gd:333`); a sibling plugin's callback then did not receive those paths, although the files were bundled.

### The Trap / Symptom
The addon wrote both `.dstex` into `textures.dspkg` and logged `Skipped texture payload from PCK` for each, while a second export plugin recorded zero `.dstex` paths in `_export_file`. Asserting the archive from that log produced eleven false failures; the archive was correct the whole time.

### The Rule
Never assert exported contents from an export plugin's `_export_file` log when another plugin may call `skip()`; read the artifact or the archive instead.

So: open `textures.dspkg` with `DSPkg.get_entries()` or scan the pack. Keep `_export_file` logging for platform-level file sets (scenes/resources), never for the archive. Related: Q50.

## 61. Export Selection Is `export_files`; `customized_files` Is Dedicated-Server Only

### The Engine Reality
`EditorExportPlatform::export_project_files()` has one branch per mode (`editor_export_platform.cpp:1324-1334`): all-resources scans everything; exclude erases `get_files_to_export()` from that set (`:1327-1332`); selected scenes/resources resolve the dependencies of `get_files_to_export()`; only `EXPORT_CUSTOMIZED` reads `customized_files` (`:1333`). `save_config()` writes `export_files` for the non-customized modes and `customized_files` for the customized one (`editor_export.cpp:78-86`).

### The Trap / Symptom
Writing `customized_files={"res://assets/unused.dstex": "remove"}` under `export_filter="exclude"` excluded nothing; all four fixture files were exported. The same list under `customized` (with `dedicated_server=true`) did exclude the file. Also, entries in `export_files` whose file no longer exists are dropped at load (`editor_export.cpp:370`), so renaming a scene silently changes the export set.

### The Rule
Always write export selection to `export_files` for selected-scenes, selected-resources and exclude modes; `customized_files` only drives dedicated-server mode.

- selected scenes / resources / exclude → `export_files=PackedStringArray("res://...")`
- "Export as dedicated server" → `export_filter="customized"`, `dedicated_server=true`, `customized_files={"res://x": "remove"}`

Measure the resulting set from the artifact, not from preset intent. Related: Q57, Q60.

## 62. Changing `SCRIPT_AES256_ENCRYPTION_KEY` Does Not Rebuild the Generated Key Under Ninja

### The Engine Reality
Godot generates `core/script_encryption_key.gen.cpp` in `core/SCsub`:

```
gen_encrypt = env.CommandNoCache(
    "script_encryption_key.gen.cpp",
    env.Value(os.environ.get("SCRIPT_AES256_ENCRYPTION_KEY")),
    env.Run(core_builders.encryption_key_builder),
)
```

`encryption_key_builder` starts with `src = source[0].read() or "0" * 64`, so an unset or empty key becomes 64 ASCII zeros, i.e. 32 zero key bytes. Under `ninja=yes`, SCons translates the `env.Value` source into a Ninja target that depends on `.ninja/scons_daemon_dirty`; Ninja only compares file mtimes, so changing the environment variable alone does not invalidate the existing `.cpp`.

### The Trap / Symptom
The keyed run prints `*** IMPORTANT: Compiling Godot with custom SCRIPT_AES256_ENCRYPTION_KEY`, but the binary is still the previous keyless build. In this project a 4.7.2 template that was supposed to carry `fc13d94a…` accepted a zero-key PCK and returned 32 garbage bytes for the real key; with `encrypt_directory=true` it failed with `Can't open encrypted pack directory`. After deleting the generated file and rebuilding, the 32 key bytes were present at offset 51,106,768 and the positive read passed 20/20.

### The Rule
When rebuilding a Godot template with a different `SCRIPT_AES256_ENCRYPTION_KEY`, delete `core/script_encryption_key.gen.cpp` and the template binary, or build without `ninja=yes`; Ninja tracks only timestamps and will reuse the old keyless generated file.

```
rm core/script_encryption_key.gen.cpp
rm bin/godot.windows.template_release.x86_64*
# rerun the full scons command with SCRIPT_AES256_ENCRYPTION_KEY set
```

Verify the result from the binary (search the 32 key bytes) or with a zero-key-vs-real-key export; do not trust the banner. Related: Q57, Q58.

## 63. `SceneTree.quit()` in an Autoload Defers and Can Be Overridden

### The Engine Reality
`SceneTree.quit(exit_code)` quits "at the end of the current iteration". Autoloads are added to `root` in project order; `_enter_tree` runs as each is added, then their `_ready` runs before the main scene. A `quit()` from an early autoload marks the tree for exit but does not abort the rest of that iteration.

### The Trap / Symptom
With three autoloads, the first calling `quit(1)` in `_enter_tree`, the observed order was `A_ENTER, A_QUIT, B_ENTER, C_ENTER, A_READY, B_READY, C_READY, MAIN_READY`, and the process exited 0 because the main scene's `quit(0)` came later. A validator that relies on the exit code sees success.

### The Rule
Never use `SceneTree.quit(n)` in an autoload as a stop condition or as the final exit code; it quits at the end of the current iteration, later autoloads and the main scene still run, and a later `quit(0)` overrides `n`.

Use a static `is_validating` flag checked by the addon's own entry points, and a sentinel line/file as the verdict. Set the exit code as late as possible. Related: Q09, Q29, Q54.

## 64. `OS.alert()` Blocks a Headless Run

### The Engine Reality
The 4.7 docs: `OS.alert()` "Displays a modal dialog box using the host platform's implementation. The engine execution is blocked until the dialog is closed."

### The Trap / Symptom
A headless exported player with a missing archive reached `ALERT_BEGIN` and then produced nothing; the 10 s test timeout had to kill it. The failure path looked like a hang, not an error.

### The Rule
Never call `OS.alert()` on a path reachable from a headless test or validator; it blocks until the dialog is dismissed.

Return before the alert when `DisplayServer.get_name() == "headless"` or when a validate flag is set; print the failure and the sentinel instead. Related: Q09.

## 65. `OS.get_cmdline_args()` Is Empty in an Exported Player

### The Engine Reality
In the measured 4.7.2 release player, `OS.get_cmdline_args()` returned an empty array while `OS.get_cmdline_user_args()` correctly returned everything after `--`.

### The Trap / Symptom
`--headless -- --dspkg-validate=protected` produced `USER_ARGS=--dspkg-validate=protected` and `CMDLINE_ARGS=` empty. A parser that read `get_cmdline_args()` silently saw no flags and the game booted normally instead of validating.

### The Rule
Read user flags from `OS.get_cmdline_user_args()` after `--`; never rely on `OS.get_cmdline_args()` in an exported player.

Parse `get_cmdline_user_args()` for user flags and keep engine arguments separate. Related: Q63, Q64.

## 66. A Main-Scene Load Failure Exits Before Autoload `_ready`

### The Engine Reality
`Main::start()` loads the main scene at `main/main.cpp:4767`; when that load fails, startup returns before the first main-loop iteration, so an autoload's `_ready()` never runs. The failure is reported by the resource layer, not by the game.

### The Trap / Symptom
A player whose main scene loads an archived `.dstex`, booted with `textures.dspkg` removed, printed:

```
ERROR: Failed loading resource: res://assets/used.dstex.
ERROR: Failed loading scene: res://dependent.tscn.
   at: Main::start (main\main.cpp:4767)
```

and produced zero `DSPKG_BOOT` lines plus no `[DirectStorage] Autoload initialized` line — the entire startup diagnostic gate was silent. Reproduced windowed and headless; exit 1. A main scene that does not touch archived payloads prints the sentinel normally.

### The Rule
Never diagnose a missing startup resource only from an autoload `_ready()`; if the main scene fails to load, Godot exits before autoloads report.

Either keep the boot scene free of archived payloads and load them after startup, or emit the verdict from a resource loader / SceneTree hook. Verify any startup diagnostic by booting with the archive removed. Related: Q63, Q55, Q34.

## 67. `encrypt_pck=true` Encrypts Nothing Without an Include Filter

### The Engine Reality
`encrypt_pck` only enables the per-file decision. `EditorExportPlatform::_encrypt_and_store_data()` (`editor_export_platform.cpp:377-391`) encrypts a file only when its path matches an entry in `encryption_include_filters` and matches no entry in `encryption_exclude_filters`. Filters are comma-separated globs tested with `String::matchn` against both the `res://` path and the path without the prefix. Files added with `add_file()` go through the same filters (`:1429`).

### The Trap / Symptom
A preset with PCK encryption enabled and an empty include filter writes a perfectly valid pack in which every file is plaintext — including an injected key file. An exclude filter also beats a `*` include. Measured by byte scan on 4.7.2, not inferred from source.

### The Rule
Always set `encryption_include_filters`; `encrypt_pck` only enables the per-file decision.

Keep the protected path out of the exclude filter and verify the produced pack rather than trusting the checkbox. Related: Q57, Q58, Q21.

## 68. `add_file()` from `_export_end` Is Too Late

### The Engine Reality
Extra files registered by export plugins are written inside `save_pack()` (`editor_export_platform.cpp:1475` and `:1759`). `_export_end` runs from `~ExportNotifier` (`:820-831`), i.e. when the export function's notifier is destroyed, after the pack is sealed.

### The Trap / Symptom
Calling `add_file()` from `_export_end` compiles, runs, logs nothing, and the file never appears in the pack. This looks like a filter problem and wastes time chasing the include/exclude configuration.

### The Rule
Add extra files from `_export_begin`, never from `_export_end`; the pack has already flushed its extras.

Use `add_file(path, bytes, false)`; `remap` is ignored outside `_export_file` and the file is not imported. Treat `_export_end` as reporting-only. Related: Q60, Q61.

## 69. `--main-pack` Still Honors a Project Working Directory

### The Engine Reality
Mounting a pack with `--main-pack` does not replace an already-resolved project. When the process working directory is itself a Godot project, `res://` still resolves against that project, so the pack's contents are shadowed for path lookups.

### The Trap / Symptom
A pack probe run from the repository root (which contains `project.godot` at a parent path or in the CWD) reported the local project's settings and files, not the mounted pack's. The same probe run from a neutral temp directory read the pack correctly. Measured in the export-contract gate.

### The Rule
Run `--main-pack` probes from a neutral directory; a project CWD overrides the pack for `res://`.

Assert a marker that only the pack contains. Related: Q56, Q65.

## 70. A Plugin `Window` Has No `panel` Theme Item

### The Engine Reality
`Window` binds exactly two styleboxes from its theme: `embedded_border` and `embedded_unfocused_border` (`scene/main/window.cpp:3688`). There is no `panel` item for the window body. The editor themes only define `panel` for `AcceptDialog` and `PopupPanel` (`editor/themes/theme_classic.cpp:1040`, `editor/themes/theme_modern.cpp:1026`). `AcceptDialog` gets a body by creating an internal `Panel` and overriding `panel` with its own `theme_cache.panel_style` (`scene/gui/dialogs.cpp:98`).

So a bare `Window` in an editor plugin has no editor-themed item to fall back on for its background, while every `Control` inside it does have one. The result is a dialog that is half editor-themed.

### The Trap / Symptom
The pack scan dialog showed editor-styled buttons and labels on a default-grey body, in both dark and light editor themes. Assigning the theme looked correct and verifiably round-tripped:

```text
[DSPkg] pack scan dialog theme: hint=true theme_null=false roundtrip=true panel=true scale=1.00
```

Three fix rounds were spent on wiring before the theme definition was read: explicit `theme` on the `Window`, `theme` on the root `VBoxContainer`, `EditorFileDialog` (refuses to instantiate outside the editor), `wrap_controls` (does not resize an unshown window), and parenting to `EditorInterface.get_base_control()` instead of `EditorInterface.popup_dialog_centered()` (the helper parents to `EditorNode`, a plain `Node`, and theme chains break at non-`Control`/non-`Window` nodes — `scene/theme/theme_owner.cpp:135`).

None of those addressed the missing item.

### The Rule
Always place a full-rect `Panel` behind a plugin `Window`'s content and override its `panel` with `AcceptDialog/panel`; assigning `theme` to the window cannot style its body.

Keep the layout margins from that same stylebox so content sits where the editor's dialogs put it. Our implementation: `project/addons/directstorage_accelerator/tools/dspkg_pack_scan_dialog.gd` `_apply_editor_theme()`.

```gdscript
var dialog_style: StyleBox = EditorInterface.get_editor_theme().get_stylebox("panel", "AcceptDialog")
if dialog_style != null:
    _background.add_theme_stylebox_override("panel", dialog_style)
    _layout.offset_left = dialog_style.get_margin(SIDE_LEFT)
    # ... all four sides
```

The `Panel` must be a full-rect child placed before the content so it stays behind it. Related: Q66 for editor-run lifecycle ordering.

## 71. An Imported Source Is Never Packed

### The Engine Reality
A file with an `.import` companion is not shipped as itself. The exporter walks the project, and any path that has a valid import is replaced by its imported artifact plus a `.remap` entry, so `res://x.glb` resolves to `.godot/imported/x.glb-<hash>.scn` inside the pack. `include_filter` does add matching files from disk (`editor_export_platform.cpp:1370` → `_edit_filter_list` → `_edit_files_with_filter`), but the substitution happens afterwards, so the filter loses. There is no "keep the source" option on the scene importer.

### The Trap / Symptom
The 3D benchmark failed in every exported build with:

```text
[DS 3D Benchmark] ERROR: Model file not found: res://test_assets/steampunk_underwater_explorer.glb
```

Measured on a plaintext export mounted in a neutral project:

```text
RES res://test_assets/steampunk_underwater_explorer.glb  file=false  loader_exists=true  load=true
```

`include_filter="README.txt,*.glb"` had been in the preset the whole time and never shipped the file. The size arithmetic proves it independently: an 80 MB pack cannot contain a 50 MB source plus everything else.

The first probe missed this because it ran with `--path project`, so `res://` fell through to the live project and reported `file=true` — see Q69.

### The Rule
Never rely on `include_filter` to ship an imported source (`.glb`, `.png`); Godot substitutes the imported artifact plus a `.remap`, so raw bytes need a non-imported extension or `add_file()`.

For a reader that needs raw bytes, give the asset a non-imported name and match that in the include filter:

```text
res://test_assets/raw/steampunk_underwater_explorer.glb.bin   # not imported
include_filter="README.txt,*.bin"
```

Or inject the file with `add_file()` from `_export_begin` (Q68). Note that `include_filter="*.glb"` is a silent no-op — worth deleting from presets because it reads like it works.

## 72. `add_autoload_singleton()` Marks the Editor Unsaved

### The Engine Reality
`EditorPlugin::add_autoload_singleton()`/`remove_autoload_singleton()` do not write a project setting directly; they hand off to `EditorAutoloadSettings::autoload_add()`/`autoload_remove()` (`editor/settings/editor_autoload_settings.cpp:762`, `:806` in 4.7.2), and both wrap the change in an `EditorUndoRedoManager` action:

```cpp
EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
undo_redo->create_action(TTR("Remove Autoload"));   // no history id -> GLOBAL_HISTORY
...
undo_redo->commit_action();
```

`EditorNode::_update_unsaved_cache()` ORs `is_history_unsaved(GLOBAL_HISTORY)` into the title star (`editor_node.cpp:386-388`), the quit flow gates "Save modified resources before closing?" on the same flag (`:3779`), and committing sets `saved_version = UNSAVED_VERSION` (`editor_undo_redo_manager.cpp:390-394`), which `is_history_unsaved()` treats as permanently dirty. Only a save path calls `set_history_as_saved(GLOBAL_HISTORY)` (`editor_node.cpp:2426`), and that method is not exposed to GDScript, so a plugin cannot clear it.

### The Trap / Symptom
Disabling an addon that manages its own autoload left the editor showing a modified project, with no scene touched:

```text
(*) test_3d_steampunk_benchmark.tscn - DirectStorage Accelerator Demo - Godot Engine
```

Quitting then asked to save modified resources, and undo did nothing because the action lives in the global history rather than the scene's. The mark is easy to misread as "the scene has unsaved edits", which sends you looking at whatever was edited last — and because `project.godot` may not be written yet, the autoload state on disk can disagree with the editor while the star is showing.

### The Rule
Never call `add_autoload_singleton()`/`remove_autoload_singleton()` from a plugin's enable/disable path; write the `autoload/<name>` entry with `ProjectSettings.set_setting()`/`clear()` + `save()` instead.

```gdscript
func _sync_autoload(p_name: String, p_path: String) -> void:
    var key := "autoload/" + p_name
    if p_path.is_empty():
        if not ProjectSettings.has_setting(key):
            return
        ProjectSettings.clear(key)
    elif ProjectSettings.has_setting(key):
        return                      # already registered; keep its *uid:// form
    else:
        ProjectSettings.set_setting(key, "*" + p_path)
        ProjectSettings.set_as_basic(key, true)
    ProjectSettings.save()
```

Our implementation: `project/addons/directstorage_accelerator/scripts/ds_plugin.gd` `_sync_autoload()`, called from `_enter_tree()` (legacy purge + register) and from `_disable_plugin()` (unregister). Never from `_exit_tree()`: that runs on a normal editor shutdown too, and unregistering there deletes the `autoload/<name>` entry from `project.godot` whenever the editor closes, so the next standalone or exported run has no autoload node at all. `_disable_plugin()` is the only callback the engine fires exclusively for a user-initiated disable (`EditorNode::set_addon_plugin_enabled()` calls `disable_plugin()` before `remove_child()` only when `p_config_changed` is true; `EditorNode::unload_editor_addons()` on quit passes `false`). Edits made through the Project Settings -> Autoload UI still take the undoable path; only the plugin's own enable/disable writes bypass it, which is the intent.

Two accepted costs: the Project Settings -> Autoload list does not refresh until it is reopened, and the live autoload node stays in the editor tree until restart. Also note that editor plugin scripts are not hot-reloaded from disk (there is no reload hook in `editor_node.cpp`), so restart the editor before judging a change to plugin startup code — otherwise you are testing the old script.

## 73. A Runtime-Assigned Editor Theme Is Serialized Into the Scene

### The Engine Reality
Saving a scene runs `PackedScene::pack()`, which walks every node property and stores any `Resource` reference that differs from the class default. The editor theme is built at runtime by `EditorThemeManager` and has no `resource_path`, so it cannot become an `[ext_resource]`; it is inlined as a `[sub_resource]` together with its whole dependency graph. `Node::NOTIFICATION_EDITOR_PRE_SAVE` (9001) and `NOTIFICATION_EDITOR_POST_SAVE` (9002) fire immediately before and after that write, in the editor only.

### The Trap / Symptom
A dialog scene that was ~100 lines on disk became 6,826,613 bytes / 22,550 lines, with Godot warning that the text scene is large:

| sub-resource | count | bytes |
| --- | --- | --- |
| `FontFile` (embedded TTFs: Noto/Vazirmatn/Inter/JetBrains Mono) | 29 | 4,158,429 |
| `Image` (256x256 `LumAlpha8` MSDF atlases) | 10 | 1,749,190 |
| `DPITexture` (inline SVG icon sources) | 1,045 | 680,751 |
| `EditorTheme` (320 colors / 428 styles / 1,200 icons) | 1 | 149,312 |

The fingerprints pointed at the script rather than an import: `theme = SubResource("EditorTheme_...")` sat on exactly the three nodes `_apply_editor_theme()` assigns it to, `theme_override_styles/panel` was the `AcceptDialog` panel StyleBox, and the four layout offsets equaled that StyleBox's margins. The editor was faithfully recording the live tree, so the file looked corrupt while the saved text had never contained those resources.

### The Rule
Never leave `EditorInterface.get_editor_theme()` (or a StyleBox taken from it) assigned to a node property in a scene the editor can save; clear it in `NOTIFICATION_EDITOR_PRE_SAVE` (9001) and restore it in `NOTIFICATION_EDITOR_POST_SAVE` (9002).

```gdscript
func _notification(what: int) -> void:
    if not Engine.is_editor_hint() or not is_node_ready():
        return
    if what == NOTIFICATION_EDITOR_PRE_SAVE:
        theme = null
        _layout.theme = null
        _file_dialog.theme = null
        _background.remove_theme_stylebox_override("panel")
    elif what == NOTIFICATION_EDITOR_POST_SAVE:
        _apply_editor_theme_look()      # theme half only, never the UI-scale resize
```

Deleting the baked block without the guard does not stick: opening the scene re-runs `_ready()`, so the next save re-adds ~6.4 MB. Saving the scene as `.scn` only hides the diff. Related: Q70 (the window panel that needs the theme in the first place), Q72.

The same leak applies to any state a `@tool` script computes at load time — `content_scale_factor`/`size`/`min_size` multiplied by `EditorInterface.get_editor_scale()` are node properties too, so saving from a non-1.0 editor scale bakes the multiplied values and they compound on the next open.

## 74. `slices.size() < 7` Is a Filesystem-Cache Parse, Not a Scene Fault

### The Engine Reality
`EditorFileSystem::_scan_filesystem()` (`editor/file_system/editor_file_system.cpp:409` in 4.7.2) first loads the on-disk scan cache at `:425-488`. Line 1 is the import-settings hash, `::<dir>::<uid>` lines are directory headers, and every other line is one file:

```text
name::type::uid::mtime::import_mtime::import_valid::group::<name<>extends<>icon<>abstract<>tool<>import_md5<>dest_paths>::deps
```

Field 7 must split into 7 `<>` slices; `:476` is `ERR_CONTINUE(slices.size() < 7)`, so a malformed record is skipped and its file rescanned. `_save_filesystem_cache()` always writes 7, and the format is identical from 4.5 through 4.7.3, so a version difference is not a plausible cause — a truncated or interleaved write is.

### The Trap / Symptom
Right after an unrelated edit, the editor printed one of these per bad record at startup:

```text
ERROR: (233) editor/file_system/editor_file_system.cpp:476 - Condition "slices.size() < 7" is true. Continuing.
```

The obvious reading — "the scene I just changed corrupted the project" — is wrong, and timestamps settled it here: the scene was written at 20:03:54, the editor started at 20:07:35 and rewrote a clean cache 5 s later, while the malformed cache had been written by the previous session before 19:53. Scene size or content never enters that file; a record holds only name, type, uid, timestamps, import flags and dependency paths, so a 6.5 MB -> 3.4 KB scene rewrite cannot move the needle.

### The Rule
Never debug scene or resource content for `editor_file_system.cpp:476 Condition "slices.size() < 7"`; it is a malformed record in `.godot/editor/filesystem_cache10`, so rescan or delete the cache instead.

`ERR_CONTINUE` makes the damage cosmetic: one error per bad record, only on a full scan, and the scan that follows rewrites the file. Confirm by parsing the current cache (233 records, 0 malformed in our case), or delete `.godot/editor/filesystem_cache10` to force a clean rebuild. Related: Q56 (another stale editor-cache trap), Q47.

## 75. Slow Quit Is Scene Teardown Under Memory Pressure, Not Eviction

### The Engine Reality
`SceneTree::_flush_delete_queue()` destroys every node queued for deletion in one frame, so a level swap pays `Object` destruction plus the RenderingServer release of everything those objects owned, on the main thread, in the same frame before the draw. Measured on the demo side (Godot 4.7.2, D3D12, GTX 1050 Ti sharing the desktop with the editor), that single flush is where the exit time goes - not the texture cache, not eviction, and not the editor plugin's autoload bookkeeping.

### The Trap / Symptom
The same ~976 MiB level produced wildly different exit times with an unchanged byte count:

| condition | tree flush |
| :--- | :--- |
| RAM headroom | 16.7-17.6 ms |
| commit charge >= RAM, pagefile on an almost-full drive | 6.9 s and 13.4 s (two separate runs) |

The draw that follows stayed in the tens of milliseconds in every run, so the whole variance sits in the flush. For scale, the addon's eviction path on the same machine released 118 textures via `DSTextureCache.clear_unused()` in 859 us on the main thread plus one ~24 ms render-thread drain frame. Because it also releases level textures, it took the blame for the multi-second quit for a while - three orders of magnitude apart, and the byte count never changed.

### The Rule
Never diagnose a slow quit from the texture cache, eviction count, or plugin state; measure the tree flush separately from the draw and compare the freed byte count between runs.

Split the exit frame with `RenderingServer.frame_pre_draw` (or log around the flush) and correlate with `RENDER_VIDEO_MEM_USED` / `OS.get_static_memory_usage()` deltas. Identical bytes with wildly different wall time is the environment: check commit charge and where the pagefile lives before touching code. To amortize a genuinely large teardown, detach the level immediately and free a bounded number of nodes per frame instead of `remove_child()` + `queue_free()` on the whole subtree in one frame.

The addon deliberately does not observe scene teardown and emits no warnings about it (documented in the addon README under "Async Prewarm: Who Owns What"), so a game that wants the cost spread must drive that itself. Related: Q43 (eviction semantics), Q63 (exit ordering with an autoload `quit()`).

## 76. Cold `--import --quit` With a GDExtension Exits 0xC0000005

### The Engine Reality
On a cold first import the editor queues extension documentation generation as a deferred main-thread call (`editor/doc/editor_help.cpp:3041`, `callable_mp_static(&EditorHelp::_gen_extensions_docs).call_deferred()`, commented "Extensions' docs are not cached"). Teardown then frees the data that call depends on: `EditorNode::~EditorNode()` calls `EditorHelp::cleanup_doc()` (`editor/editor_node.cpp:9640`), which does `memdelete(doc); doc = nullptr;` (`editor_help.cpp:3365-3369`). `SceneTree::finalize()` deletes the root (`scene/main/scene_tree.cpp:863`), and the message queue is flushed during `Main::cleanup()`'s `delete_main_loop()` (`main/main.cpp:5259`), so the still-queued callable runs against a null `doc`.

Symbolized stack (custom build of the same commit `ed1daf0bf`, `debug_symbols=yes dev_build=yes lto=none`; the shipped binary reproduces the same fault at its own `+0x53fd042`):

```text
read on 0x8   godot.windows.editor.dev.x86_64.exe + 0x230d133
[0] core/templates/hash_map.h:561        HashMap::operator[] on a null 'this'
[1] editor/doc/doc_tools.cpp:452         class_list[name] = DocData::ClassDoc();
[2] editor/doc/editor_help.cpp:3084      _gen_extensions_docs(): doc->generate(...)
[3] core/variant/binder_common.h:530
[5] core/object/callable_mp.h:245
[6] core/variant/callable.cpp:58         Callable::callp
[8] core/object/message_queue.cpp:266    MessageQueue::flush -> _call_function
[9] main/main.cpp:5259                   Main::cleanup -> delete_main_loop()
```

### The Trap / Symptom
`godot --headless --path <fresh project, no .godot> --import --quit` writes `.godot/`, prints `[ DONE ] loading_editor_layout`, and then exits `-1073741819` (`0xC0000005`) with no engine backtrace. It is an engine defect, not ours: the extension DLL is already unloaded by `GDExtensionManager::shutdown()` (`main.cpp:5234`) before the flush, and renaming the `.gdextension` file away makes the same project exit 0.

Three properties make it look like our regression and waste a day if you do not know them:

- **Any GDExtension triggers it.** With an empty extension class list `DocTools::generate()` never touches `class_list`, so the null `doc` is harmless; the first extension class makes the loop body run. Reproduced 2/2 with this addon, 2/2 with an unrelated `gdextest` library, and 0/2 with no addon or without the `.gdextension` file.
- **Cold first import only.** Re-running the identical command on the same project (now with `.godot/`) exits 0, because no doc-generation callable is pending. Normal interactive editor use is unaffected - the queue is flushed every frame while `doc` is still alive.
- **The engine stays silent.** On Windows the crash handler is a `__try/__except` around `main` (`platform/windows/godot_windows.cpp:145`), and the fault happens inside `Main::cleanup`, so no `CrashHandlerException` / `Dumping the backtrace` output is produced on the shipped binary or on a `dev_build=yes` build with a PDB.

Measured cost: every CI job that imports a fresh checkout of a project containing a GDExtension reports failure on a successful import (exit code only; no artifacts are wrong).

### The Rule
Gate on your own sentinel (`DSPKG_BOOT`, the scan output, or an `[ DONE ]` line), never on `$LASTEXITCODE`, and pre-seed `.godot/extension_list.cfg` or run gates against a warm fixture - which is what the export-contract harness already does, and why it never hit this.

Upstream fix options, cheapest first: return early when `doc == nullptr` in `_gen_extensions_docs()` / `generate_doc()`; cancel the queued documentation callables in `EditorHelp::cleanup_doc()`; or do not defer doc work while the editor is quitting.

To reproduce, build a fresh project with no `.godot/`, add any `addons/<name>/<name>.gdextension` plus the library it names (nothing needs to be enabled, autoloaded or referenced by a scene), then run `godot --headless --path <project> --import --quit`: the first run exits `0xC0000005`, a second identical run exits `0`, and renaming the `.gdextension` away makes the first run exit `0`. The symbolized stack above comes from a custom `debug_symbols=yes dev_build=yes lto=none` build of the same commit with the debugger attached after the import starts - attaching from process start perturbs the teardown timing and can hide the fault. Related: Q59 (CLI harness traps), Q56 (`--script` only loads extensions from `.godot/extension_list.cfg`).

## 77. `_process(delta)` Is Not the Frame Duration; Long Frames Are Cut

### The Engine Reality
The delta handed to `Node._process()` is not the real frame duration. `Main::iteration()` builds it from `MainTimerSync::advance()` (`main/main.cpp:4938`), and the decisive cut is `main/main.cpp:4955-4959`: when `fixed_fps == -1` and a frame needed more physics steps than `physics/common/max_physics_steps_per_frame` (default 8, `main/main.cpp:2253`), it subtracts `(physics_steps - max_physics_steps) * physics_step` from the step - a 5 s frame reaches `_process()` as ~0.15 s. That time is discarded, not deferred: `time_deficit` (`main_timer_sync.cpp:441`, `:498`) is computed from the pre-subtraction value, so nothing repays it on later frames. The three `advance_checked()` clamps do not bind in that case: `:455` runs only when `get_average_physics_steps()` reports more than 3 consistent frames (after a long frame `advance_core` has already rewritten `typical_physics_steps` to the observed ~299 steps, which makes it bail at `i == 1`), `:461` is a `+/- physics_jitter_fix * physics_step` band (default 0.5 -> +/- 8.3 ms, `main/main.cpp:2254`) around the raw value, and `:464`'s upper bound is the raw value itself. The observable ceiling is therefore about `(max_physics_steps + leftover) * physics_step` = 133-150 ms at 60 Hz, independent of how long the frame really was.

### The Trap / Symptom
A background teardown drain (3302 nodes, 971 MiB of VRAM released) reported both of these in the same run:

```text
[Transition] level teardown drained in 11.76 s (0.87 s of release work, ...)
[Transition] drain pacing: frames 48 | 68.8 objects/frame | frame ms median 8.7, p95 126.3, max 135.8
```

48 frames times the recorded 135.8 ms maximum is 6.5 s, so roughly 5 s of stall never appeared in any sample - and 135.8 ms is 133.3 ms (8 x 16.67) plus a 2.5 ms remainder, i.e. the `max_physics_steps_per_frame` ceiling, not the real frame. Two consequences, both real:

- A frame-budget controller that sized its per-frame work from `delta` believed every frame was ~9 ms and grew its batch to 68.8 objects/frame, which really cost ~245 ms each. The menu froze 2-3 times for the whole 11.76 s drain instead of staying smooth.
- The demo's benchmark hitch tracker built every `FREEZE (>100 ms)` entry from `delta`, so it systematically under-reported the worst frames. The only reason the earlier 12 s freeze was ever measured is that the exit benchmark probed it with explicit `Time.get_ticks_usec()` timestamps instead.

General detection heuristic: if the sum of the recorded frame times is smaller than the wall-clock duration of the same window, the samples are clamped, not missing.

### The Rule
Never measure a frame's duration, or sum deltas over a window, from `_process(delta)`; the step is vsync-smoothed and physics-clamped, and any frame needing more than `physics/common/max_physics_steps_per_frame` (default 8) has the excess subtracted and discarded. Measure with `Time.get_ticks_usec()` between frames.

```gdscript
func _process(_delta: float) -> void:
	var now := Time.get_ticks_usec()
	var frame_ms := 0.0
	if _last_frame_usec > 0:
		frame_ms = float(now - _last_frame_usec) / 1000.0
	_last_frame_usec = now
```

Re-arm `_last_frame_usec = 0` whenever the sampler restarts (benchmark start, drain start) so a long idle gap is not attributed to the first frame, and drop the first interval when the restart point is not itself a frame boundary - `main.gd` arms its drain clock inside the scene swap, so interval #1 covers only the remainder of that swap (which includes the incoming scene's `_ready()`) and must not seed the menu baseline, or the baseline stays artificially low for the whole drain. Use `delta` only for things that genuinely want smoothing (animation, EMA baselines). Implemented in `tps-demo/main/main.gd` (`_measure_drain_frame()`, `_drain_last_tick_usec`, `_record_drain_frame()`) and `tps-demo/menu/settings.gd` (`_last_frame_usec`). Any per-frame budget expressed in milliseconds (Q13, Q41) must use the same measurement or it will overshoot by the same factor. Related: Q75 (the teardown this was measured on), Q41 (frame hitching from batch sizes).

## 78. `RENDER_VIDEO_MEM_USED` Is a Stale Per-Frame Throttling Signal

### The Engine Reality
`Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED)` is refreshed after the draw, so a value read in `_process()` describes the previous frame's state, not the current one. Destroying nodes in `_process` queues `free_rid` work that the render thread executes inside the same frame's draw, which means the byte drop caused by this frame's batch is only visible to a sample taken one frame later - and is mixed with whatever the new scene allocated in between.

### The Trap / Symptom
A teardown throttle written as "release at most `TEARDOWN_VRAM_CAP_MIB = 4.0` MiB per frame", gated on `last_sample - current_sample >= cap`, actually released 975 MiB across ~104 frames, i.e. **9.4 MiB per frame - 2.3x over its own cap**. The under-read meant the gate rarely tripped, so the real control was the unrelated 32-objects-per-frame limit, and each frame's draw carried ~134 ms of render-thread destruction. The same counter produced the nonsense `released ... in 0.000 s = 0.0 MiB/s` line in the exit benchmark, because the frame-1 sample still read the pre-teardown value.

### The Rule
Never throttle per-frame GPU work on `Performance.get_monitor(RENDER_VIDEO_MEM_USED)` read in `_process`; the counter is refreshed after the draw, so a per-frame delta under-reads and a byte cap behaves like a much larger one - and the release itself is spent by the render thread inside that same draw, after your throttling decision.

Sample it only at window boundaries (drain start, drain end, menu arrival) for reporting, where the level-minus-menu delta is valid, and drive any pacing from measured frame time instead (Q77). Implemented in `tps-demo/main/main.gd`: `_drain_vram_start_mib` / `_menu_vram_mib` are boundary samples for the report, while `DRAIN_TARGET_FRAME_MSEC` pacing sizes the batch from real frame duration. Related: Q77 (the pacing that replaced it), Q75 (memory-pressure teardown), Q35 (eviction/capacity accounting).

## 79. `Texture2DRD` Has No Size Override

### The Engine Reality
`Texture2DRD::get_width()` / `get_height()` return an internal `size` member copied from the
device texture format in `_set_texture_rd_rid()` (`scene/resources/texture_rd.cpp`), and the
class exposes no setter for it. `ImageTexture`, by contrast, ships a bound
`set_size_override(Size2i)` that updates its `w`/`h` and forwards to
`RenderingServer::texture_set_size_override()` (`scene/resources/image_texture.cpp`), and
`CompressedTexture2D::load()` does the same pairing manually (`w = lw; h = lh;` plus the RS
override). So of the three 2D texture types, `Texture2DRD` is the only one that cannot present
logical dimensions different from its storage dimensions. Two further traps compose with it:
the RS override never affects `Texture2DRD::get_width()` (the getter never consults the
server), and `RenderingDevice` RIDs live in a separate ID space from `RenderingServer` RIDs,
so passing a device RID to `texture_set_size_override()` risks overriding an unrelated live
RS texture on numeric collision instead of failing loudly.

### The Trap / Symptom
A 100x50 NPOT source imports to a 100x52 block footprint (4-pixel alignment, same rounding
Godot's own `cvtt`/`etcpak`/`betsy` codecs apply). The CPU load path reports 100x50; the GPU
`Texture2DRD` wrap of the identical payload reports 100x52 with no API to correct it. During
implementation the GPU branch first applied the override to the device RID — wrong namespace,
caught only by reading the engine source before any symptom appeared.

### The Rule
Never expect `Texture2DRD.get_width()` to report logical dims or pass a device RID to `texture_set_size_override`; use `ImageTexture.set_size_override` on CPU paths and document storage-dims on GPU wraps.

```cpp
godot::Ref<godot::ImageTexture> img_tex = godot::ImageTexture::create_from_image(image);
img_tex->set_size_override(godot::Vector2i(logical_w, logical_h));
```

GPU wraps override the RS wrapper RID from `get_rid()`, and only where the wrap already
executed (the render thread — `set_texture_rd_rid` is async elsewhere, so preloader GPU
completion documents storage dims). The addon exposes the CPU escape as `compatibility/as_image_texture`: enabling it forces an `ImageTexture` with `set_size_override`, which is the supported way to get authoring dimensions for non-multiple-of-4 sources. Implemented in
`src/integration/ds_resource_format_loader.cpp` (sync loader) and
`src/integration/ds_preloader.cpp` (prewarm build). Related: Q05 (Texture2DRD CPU-pixel
gaps), Q27 (Texture2DRD RID ownership), Q36 (which RID to validate). Upstream report:
`.agents/godot-bug-reports/texture2drd-no-size-override.md`.
