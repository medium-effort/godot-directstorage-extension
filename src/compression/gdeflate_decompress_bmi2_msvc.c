#if defined(_MSC_VER) && defined(_M_X64)

/*
 * Compile libdeflate's GDeflate decompressor with /arch:AVX2 (which enables
 * BMI2 + AVX2 codegen on MSVC) under distinct symbol names so GDeflateDecompress
 * can select it at runtime after verifying CPUID support.
 */
#define libdeflate_gdeflate_decompress libdeflate_gdeflate_decompress_bmi2_msvc
#define libdeflate_gdeflate_decompress_ex libdeflate_gdeflate_decompress_ex_bmi2_msvc
#define libdeflate_alloc_gdeflate_decompressor libdeflate_alloc_gdeflate_decompressor_bmi2_msvc
#define libdeflate_free_gdeflate_decompressor libdeflate_free_gdeflate_decompressor_bmi2_msvc

#include "gdeflate_decompress.c"

#endif
