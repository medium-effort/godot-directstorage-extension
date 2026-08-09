# Supported Extension Artifacts

The manifest declares these build targets:

- Windows x86_64: `template_debug` and `template_release`, with DirectStorage runtime DLLs.
- Linux x86_64: `template_debug` and `template_release`, using the software GDeflate fallback.
- macOS universal: `template_debug` and `template_release`, covering x86_64 and arm64 with the software GDeflate fallback.

Other architectures and platforms are not declared until matching artifacts are built and tested. They must not be inferred from the fallback backend alone. DirectStorage runtime DLLs are Windows-only dependencies.
