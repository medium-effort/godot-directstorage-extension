# Godot Architectural Quirks, Edge Cases & Real-World Lessons

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
In Godot 4.x, [RenderingDevice](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html) methods—such as `texture_create()`, `free_rid()`, and `get_driver_resource()`—are **strictly thread-affine to the render thread**. Calling them directly from background worker threads triggers debug assertions or graphics driver faults (see [Godot Thread-safe APIs](https://docs.godotengine.org/en/stable/tutorials/performance/thread_safe_apis.html)).

### The Trap: Marshaling via `call_on_render_thread()`
To bypass this limitation, an earlier experimental implementation marshaled texture creation and driver queries from worker threads to the render thread using `RenderingServer::call_on_render_thread()`, waiting on a `std::condition_variable`.

In Godot's default single-safe thread model (`THREAD_MODEL_SINGLE_SAFE`), **callables posted to the render thread are only drained once per frame when the main thread draws**:
- Texture 1 creation wait: ~16.6 ms (1 frame at 60 FPS)
- Texture 1 driver query wait: ~16.6 ms (1 frame)
- Texture 2 creation wait: ~16.6 ms (1 frame)...

In a real game project like `tps-demo` with ~250 textures:
```text
250 textures * ~60 ms cross-thread wait = ~15,000 ms (15 to 17 seconds of frozen loading screen!)
```

### The Invariant Rule
Direct GPU texture decompression into [Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html) is only viable **on the render thread** (synchronous loads or runtime streaming systems).
Off-render-thread background loads (`load_threaded_request`) **MUST enforce the thread-affinity gate**:
```cpp
if (!rs || !rs->is_on_render_thread()) {
    // Clean, immediate fallback to CPU GDeflate + DirectStorage BypassIO memory pipeline.
}
```
This CPU memory pathway decompressing into [ImageTexture](https://docs.godotengine.org/en/stable/classes/class_imagetexture.html) takes ~0.5 ms per texture, loading 250 textures in ~125 ms total without a single frame stall.

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
- **Light3D Projectors**: Projector textures on OmniLight3D and SpotLight3D require CPU image formats for filtering setup and will push errors if assigned a `Texture2DRD`.
- **Decals**: Some decal blend modes query texture data formats through CPU image representation.
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
Passing `--headless` to the Godot binary forces the Dummy rendering driver (`RenderingServer::is_low_end()`), completely bypassing D3D12 device creation. Any automated test asserting real DirectStorage GPU decompression or VRAM readback will fail.

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
Windows allows renaming a running DLL even while it is actively mapped into memory. Build scripts (`build_remote.ps1`) should implement the safe-rename pattern:
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

