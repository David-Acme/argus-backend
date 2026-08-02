#pragma once

#include <string>

namespace qr_render
{

// Renders `text` as a scannable ASCII QR code (Unicode block glyphs, 2 chars
// per module, 4-module quiet zone). Returns a multi-line string.
std::string asciiQr(const std::string& text);

} // namespace qr_render
