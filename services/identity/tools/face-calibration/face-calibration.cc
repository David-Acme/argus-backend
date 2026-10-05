#include <shared/services/face/face-service.hxx>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace
{
std::string readFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int usage()
{
  std::cerr << "usage: argus-face-calibration <list-file>\n"
               "  Reads one image path per line and prints, per image, a tab\n"
               "  separated row: path, found, detector score, face\n"
               "  width, inter-ocular distance, yaw, pitch, sharpness, face\n"
               "  count and the embedding. Run from the backend root so models/face loads.\n";
  return 2;
}
}

int main(int argc, char** argv)
{
  if (argc != 2)
    return usage();
  FaceService::instance().init();
  if (!FaceService::instance().isLoaded())
    return 1;
  std::ifstream list(argv[1]);
  std::string path;
  while (std::getline(list, path)) {
    if (path.empty())
      continue;
    const auto face = FaceService::instance().analyzeImage(
        {.imageBytes = readFile(path), .encodeFace = false});
    std::ostringstream row;
    row << path << '\t' << (face ? 1 : 0);
    if (face) {
      const auto& q = face->quality;
      row << '\t' << q.detectorScore << '\t' << q.faceWidthPx << '\t'
          << q.interOcularPx << '\t' << q.yaw << '\t' << q.pitch << '\t'
          << q.sharpness << '\t' << face->faces;
    }
    row << '\t';
    if (face) {
      for (size_t i = 0; i < face->embedding.size(); ++i)
        row << (i ? "," : "") << face->embedding[i];
    }
    std::cout << row.str() << '\n';
  }
  FaceService::instance().shutdown();
  return 0;
}
