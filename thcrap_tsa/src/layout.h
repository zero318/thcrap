/**
  * Touhou Community Reliant Automatic Patcher
  * Team Shanghai Alice support plugin
  *
  * ----
  *
  * Text rendering layout.
  */

#pragma once

/// Tokenization
/// ------------
// Matches at most [len] bytes of layout markup at the beginning of [str].
// Returns an array with the layout parameters and, optionally,
// the full length of the layout markup in [str] in [match_len].
json_t* layout_match(size_t *match_len, const char *str, size_t len);

// Split the string into an array of tokens to render in a sequence.
// These are either strings (= direct text)
// or arrays in itself (= layout commands).
json_t* layout_tokenize(const char *str, size_t len);
/// ------------

// Calculates the rendered length of [str] on the current text DC
// with the currently selected font.
size_t __stdcall GetTextExtent(const char *str);

// GetTextExtent with an additional font parameter.
// [font] is temporarily selected into the DC for the length calcuation.
size_t __stdcall GetTextExtentForFont(const char *str, HGDIOBJ font);

int layout_init(HMODULE hMod);
void layout_exit();
