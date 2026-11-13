// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/conformer/
// inc/conformer_tokenizer.h (Apache-2.0, Copyright 2026 lspr98): the text
// goes into a caller's char buffer instead of a std::string, and only the
// frames [first, end) are decoded (overlapping clips, plugin.cpp).

#ifndef CONFORMER_TOKENIZER_H_
#define CONFORMER_TOKENIZER_H_

#include <cstddef>
#include <cstdint>

#include <tlib_tensor.h>

namespace conformer {

// Blank token for CTC decoding (the argmax index after the 1024-token vocabulary)
inline constexpr uint32_t kBlankToken{1024};

/**
 * Decodes frames [first, end) of a token sequence into a text buffer. A
 * token repeated from frame first - 1 is not emitted again (CTC collapse
 * across the range start).
 *
 * @param   tokens  [n] uint32_t tensor, argmax of the output logits
 * @param   first   First frame to decode
 * @param   end     One past the last frame to decode (at most n)
 * @param   buffer  Receives the text, NUL-terminated (truncated to cap - 1 bytes)
 * @param   cap     Size of buffer
 *
 * @returns The text's length in bytes
 */
size_t Decode(const tlib::TensorView<uint32_t> & tokens, uint32_t first, uint32_t end, char * buffer, size_t cap);

} // conformer

#endif
