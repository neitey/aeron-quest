/* Wrapper that compiles the vendored bcdec decoder (single translation unit:
 * ktx2_reader.c) as a system header, silencing its C2x binary literals. */
#ifndef AERON_BCDEC_IMPL_H
#define AERON_BCDEC_IMPL_H
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "bcdec.h"
#endif
