#include "qr-render.hxx"

#include <qrcodegen/qrcodegen.hpp>

#include <sstream>

namespace qr_render
{

// Renders `text` as a scannable ASCII QR code using Unicode half-blocks:
// one character per module column and two module rows per line, so the
// output is compact and stays readable in a standard terminal.
std::string asciiQr(const std::string& text)
{
  const qrcodegen::QrCode qr =
      qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
  const int size = qr.getSize();
  constexpr int kQuietZone = 4;
  const int span = size + 2 * kQuietZone;

  const auto module = [&](int x, int y) {
    return qr.getModule(x - kQuietZone, y - kQuietZone);
  };

  std::ostringstream out;
  for (int y = 0; y < span; y += 2) {
    for (int x = 0; x < span; ++x) {
      const bool top = module(x, y);
      const bool bottom = y + 1 < span && module(x, y + 1);
      if (top && bottom)
        out << "\u2588";
      else if (top)
        out << "\u2580";
      else if (bottom)
        out << "\u2584";
      else
        out << ' ';
    }
    out << '\n';
  }
  return out.str();
}

} // namespace qr_render
