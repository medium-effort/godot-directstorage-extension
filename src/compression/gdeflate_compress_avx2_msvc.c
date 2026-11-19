#if defined(_MSC_VER) && defined(_M_X64)

/*
 * Compile libdeflate's GDeflate compressor with /arch:AVX2 (which enables AVX2
 * matchfinder codegen on MSVC) under distinct symbol names so GDeflateCompress
 * can select it at runtime after verifying CPUID support. gdeflate_compress.c
 * includes deflate_compress.c with HIDE_INTERFACE, so the whole compression hot
 * path lives in this translation unit; only the public GDeflate entry points
 * need renaming. The baseline libdeflate objects are untouched and remain the
 * default on every other platform and on CPUs without AVX2.
 */

#define libdeflate_alloc_gdeflate_compressor libdeflate_alloc_gdeflate_compressor_avx2_msvc
#define libdeflate_free_gdeflate_compressor libdeflate_free_gdeflate_compressor_avx2_msvc
#define libdeflate_gdeflate_compress libdeflate_gdeflate_compress_avx2_msvc
#define libdeflate_gdeflate_compress_ex libdeflate_gdeflate_compress_ex_avx2_msvc
#define libdeflate_gdeflate_compress_bound libdeflate_gdeflate_compress_bound_avx2_msvc

#include "gdeflate_compress.c"

#endif
