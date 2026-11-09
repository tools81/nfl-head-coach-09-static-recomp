#pragma once
// hc09: the menus' text at the screen's resolution (font_hd.cpp).
//
// The game rasterizes its text's glyphs as it needs them (FontFusion, from outline fonts) into 128x128 texture pages.
// Each glyph is also rasterized at Scale times the size into a copy of its page as many times larger, which the
// renderer samples in the page's place (same texture coordinates: the copy is the page, sharper).
#include <cstdint>
#include <vector>

namespace hc09::font_hd {

// The sharper copy's factor: the renderer's Scale (HC09_SCALE), 1 when off (SharpText off, or Scale 1).
uint32_t Factor();

// The version of the sharper copy of the glyph page at physical address pa: 0 when pa is not a glyph page, else
// changing whenever the copy does. Lock-free (the texture cache asks for every hit on a page-like texture).
uint64_t Version(uint32_t pa);

// The sharper copy of the glyph page at pa, if the page is width x height: rgba (0xAABBGGRR, white with the glyphs'
// coverage as alpha), Factor() times its size, and its version.
bool Image(uint32_t pa, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba, uint32_t& outWidth, uint32_t& outHeight,
           uint64_t& version);

}  // namespace hc09::font_hd
