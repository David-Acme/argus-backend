#include "qr-render.hxx"

#include <qrcodegen/qrcodegen.hpp>

#include <sstream>

namespace qr_render
{

std::string asciiQr(const std::string& text)
{
  const qrcodegen::QrCode qr =
      qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
  const int size = qr.getSize();
  constexpr int kQuietZone = 4;

  std::ostringstream out;
  for (int y = -kQuietZone; y < size + kQuietZone; ++y) {
    for (int x = -kQuietZone; x < size + kQuietZone; ++x)
      out << (qr.getModule(x, y) ? "\u2588\u2588" : "  ");
    out << '\n';
  }
  return out.str();
}

} // namespace qr_render
