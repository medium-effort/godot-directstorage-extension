# Godot Architectural Quirks, Edge Cases & Real-World Lessons

Last updated: 8 October 2026

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

## 43. Unconditional Cache `clear()` Destroys On-Screen Texture RIDs

### The Engine Reality
In GDExtensions managing custom texture resources (`Texture2DRD` or native `RenderingDevice` texture allocations), the extension owns the underlying GPU texture RIDs. Calling `DSTextureCache::clear()` immediately passes every texture RID in the cache map to `RenderingDevice::free_rid()`. However, Godot's `StandardMaterial3D` and canvas pipelines hold active shader uniform sets (Set 3) binding those texture RIDs if any mesh or UI node currently in the SceneTree references them.

### The Trap / Symptom
If `DSTextureCache.clear()` is called while a scene using any cached texture is active (for example, clearing level textures while the main menu robot model shares or references prewarmed textures like `res://player/...`), the underlying GPU texture resource is destroyed on the rendering device out from under the active material.
On the next render frame, Godot attempts to build or bind the uniform set and fails continuously:
```text
[1] uniform_set_create: Texture (binding: 1, index 0) is not a valid texture. Parameter "texture" is null.
    -> <C++ Source> servers/rendering/rendering_device.cpp:4502 @ uniform_set_create()
[2] draw_list_draw: Uniforms were never supplied for set (3) at the time of drawing, which are required by the pipeline.
    -> <C++ Source> servers/rendering/rendering_device.cpp:6033 @ draw_list_draw()
```
The visible mesh turns completely black (drawing only untextured emission or fallback shaders), flooding the debugger with thousands of error messages per second.

### The Rule
Never call unconditional `DSTextureCache.clear()` while a scene using any cached texture is visible; call `DSTextureCache.clear_unused()` to evict only unreferenced textures (`ref_count == 1`) or restrict full `clear()` to scene transitions where no actors are rendered.

For safe runtime cache eviction, `DSTextureCache::clear_unused()` scans canonical entries under `cache_mutex` using pristine reference counts before freeing:
```cpp
static bool ds_texture_cache_is_externally_held(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null() || !p_texture.is_valid()) {
		return false;
	}
	return p_texture->get_reference_count() > 1; // Held by active scene material or node outside cache
}
```
Any entry with `ref_count == 1` is removed from the cache map, alias map, and LRU list, its live twin references are safely filtered, and its GPU RID is queued for render-thread release via `ds_texture_cache_release_evicted()` (`src/integration/ds_texture_cache.cpp:573`).

In GDScript, call `clear_unused()` whenever toggling background preloads off or releasing idle level caches during active gameplay:
```gdscript
if ClassDB.class_exists("DSTextureCache"):
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if cache != null and cache.has_method("clear_unused"):
		var freed_count: int = cache.clear_unused()
		print("[Cache] Safely evicted %d unreferenced textures" % freed_count)
```
Reserve unconditional `clear()` strictly for module teardown (`~DSTextureCache()`) or blank transition screens where all old actor nodes and materials have been completely freed.

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

## 47. Defender Breaks Safe-Save Commits on Fresh res:// Files

### The Engine Reality
Godot's safe-save writes to a temp file and renames over the target at `close()` (`drivers/windows/file_access_windows.cpp:277`). In editor test runs, antivirus scan windows on brand-new `res://` files fail the rename, and `DirAccess::remove` on the same locked files fails too — both silently if return codes are ignored. A stale `textures.dspkg` then survives deletes and overwrites and poisons every later resolve in the process.

### The Trap / Symptom
```
ERROR: Safe save failed. ... running a paranoid antivirus ...
    at: _close (drivers/windows/file_access_windows.cpp:277)
```
Followed by `has_texture()` false on a just-staged archive whose staging reported success, and a stale entry dump (`mem`/`ctl` paths where `dma` paths were staged).

### The Rule
In editor test runs, treat every `res://` write as suspect: remove + copy + reopen-verify + one 500 ms retry, never trust the copy return code alone; make removal failures loud (`push_warning`, never ignored). Implemented in `tests/gpu_prewarm_tests.cpp` (`stage_gpu_dspkg`, `remove_abs`).

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