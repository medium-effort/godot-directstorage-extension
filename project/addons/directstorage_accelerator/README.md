# DirectStorage Accelerator

This addon exposes DirectStorage-backed texture loading and request APIs to Godot 4.

## Editor Workflow

Enable the `DirectStorage Accelerator` plugin. It registers the `DSBatchManager` autoload, the `DirectStorage Texture` importer, and the `DirectStorageExporter` export plugin.

The importer converts `.png`, `.jpg`, `.jpeg`, and `.webp` sources into `.dstex` resources. Its Import dock options control source compression mode, source color space, and GDeflate compression level.

During export, imported `.dstex` files are collected into `res://textures.dspkg`. The runtime resource loader can resolve texture paths from that archive without requiring each individual `.dstex` file in the exported PCK.

## Runtime API

The native classes are documented in Godot's Editor Help and support Ctrl-click navigation from GDScript. The main entry points are:

- `DSContext`: initialize the backend, enqueue `DSRequest` objects, and flush batches.
- `DSRequest`: describe a file range, compression format, destination buffer, and request status.
- `DSTex`: create or inspect individual `.dstex` files.
- `DSPkg`: create or inspect `.dspkg` archives.
- `GDeflateCompressor` and `GDeflateDecompressor`: encode and decode GDeflate payloads.
- `DSTextureCache`: inspect or manage the runtime texture cache.
- `DSErrorHandler` and `DSProfiler`: inspect asynchronous failures and throughput metrics.

The extension registers `DSContext`, `DSErrorHandler`, `DSProfiler`, and `DSTextureCache` as Engine singletons. Access them with `Engine.get_singleton("DSContext")` and the corresponding singleton name.
