// capist — converts an arbitrary image into a ruBeta cloak/cape texture.
//
// The ruBeta client (a Minecraft Beta 1.7.3 custom client) expects cloak
// textures as a PNG of a fixed canvas size. Within that canvas only the
// top-left corner is actually drawn on: the source artwork resized down,
// placed on the left, and a horizontally mirrored copy placed immediately
// to its right. The rest of the canvas stays fully transparent — that
// padding is required by the file format, it isn't visual content.
//
// Cross-platform: uses stb_image / stb_image_write instead of any OS-native
// APIs, and takes paths as command-line arguments instead of native file
// dialogs, so it builds and runs the same way on Windows, Linux and macOS.

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ruBeta cloak texture layout
// ---------------------------------------------------------------------------
// A "tile" is one 64x75 copy of the source artwork. The ruBeta client reads
// two of them side by side from the top-left of the canvas: the original,
// then a horizontal mirror of it.
constexpr int kTileWidth = 64;
constexpr int kTileHeight = 75;

// Full output canvas. The ruBeta client rejects cloak PNGs that aren't
// exactly this size, even though only the top-left 128x75 pixels
// (2 tiles wide) ever get drawn on.
constexpr int kCanvasWidth = 352;
constexpr int kCanvasHeight = 272;

constexpr int kChannels = 4; // RGBA, always — simplifies every buffer below

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA, row-major

    uint8_t &at(int x, int y, int c) {
        return pixels[(static_cast<size_t>(y) * width + x) * kChannels + c];
    }
    const uint8_t &at(int x, int y, int c) const {
        return pixels[(static_cast<size_t>(y) * width + x) * kChannels + c];
    }
};

Image MakeCanvas(int w, int h) {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<size_t>(w) * h * kChannels, 0); // transparent
    return img;
}

// Bilinear resize, RGBA -> RGBA. Good enough quality for a 64x75 target and
// has no external dependency beyond stb_image itself.
Image ResizeBilinear(const Image &src, int dstW, int dstH) {
    Image dst = MakeCanvas(dstW, dstH);
    if (src.width <= 0 || src.height <= 0) return dst;

    const float xRatio = static_cast<float>(src.width) / dstW;
    const float yRatio = static_cast<float>(src.height) / dstH;

    for (int y = 0; y < dstH; ++y) {
        float srcYf = (y + 0.5f) * yRatio - 0.5f;
        int y0 = std::clamp(static_cast<int>(std::floor(srcYf)), 0, src.height - 1);
        int y1 = std::clamp(y0 + 1, 0, src.height - 1);
        float wy = std::clamp(srcYf - y0, 0.0f, 1.0f);

        for (int x = 0; x < dstW; ++x) {
            float srcXf = (x + 0.5f) * xRatio - 0.5f;
            int x0 = std::clamp(static_cast<int>(std::floor(srcXf)), 0, src.width - 1);
            int x1 = std::clamp(x0 + 1, 0, src.width - 1);
            float wx = std::clamp(srcXf - x0, 0.0f, 1.0f);

            for (int c = 0; c < kChannels; ++c) {
                float top = src.at(x0, y0, c) * (1 - wx) + src.at(x1, y0, c) * wx;
                float bottom = src.at(x0, y1, c) * (1 - wx) + src.at(x1, y1, c) * wx;
                float value = top * (1 - wy) + bottom * wy;
                dst.at(x, y, c) = static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
            }
        }
    }
    return dst;
}

void Blit(Image &canvas, const Image &tile, int destX, int destY) {
    for (int y = 0; y < tile.height; ++y)
        for (int x = 0; x < tile.width; ++x)
            for (int c = 0; c < kChannels; ++c)
                canvas.at(destX + x, destY + y, c) = tile.at(x, y, c);
}

void BlitMirrored(Image &canvas, const Image &tile, int destX, int destY) {
    for (int y = 0; y < tile.height; ++y)
        for (int x = 0; x < tile.width; ++x) {
            int srcX = tile.width - 1 - x; // horizontal mirror
            for (int c = 0; c < kChannels; ++c)
                canvas.at(destX + x, destY + y, c) = tile.at(srcX, y, c);
        }
}

void PrintUsage(const char *argv0) {
    std::cout << "capist — converts an image into a ruBeta cloak texture\n\n"
              << "Usage: " << argv0 << " <input-image> [output.png]\n\n"
              << "  input-image   any format stb_image supports (png, jpg, bmp, gif, tga, ...)\n"
              << "  output.png    optional; defaults to \"<input-name>_c(r)apist.png\"\n\n"
              << "Output is always a " << kCanvasWidth << "x" << kCanvasHeight << " PNG, "
              << "the size the ruBeta client requires.\n";
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2 || argc > 3) {
        PrintUsage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    fs::path inputPath = argv[1];
    fs::path outputPath = (argc == 3)
        ? fs::path(argv[2])
        : inputPath.parent_path() / (inputPath.stem().string() + "_c(r)apist.png");

    int srcW, srcH, srcChannelsInFile;
    uint8_t *raw = stbi_load(inputPath.string().c_str(), &srcW, &srcH, &srcChannelsInFile, kChannels);
    if (!raw) {
        std::cerr << "Ошибка: не удалось загрузить изображение \"" << inputPath.string()
                   << "\": " << stbi_failure_reason() << "\n";
        return 1;
    }

    Image source;
    source.width = srcW;
    source.height = srcH;
    source.pixels.assign(raw, raw + static_cast<size_t>(srcW) * srcH * kChannels);
    stbi_image_free(raw);

    Image tile = ResizeBilinear(source, kTileWidth, kTileHeight);

    Image canvas = MakeCanvas(kCanvasWidth, kCanvasHeight);
    Blit(canvas, tile, 0, 0);
    BlitMirrored(canvas, tile, kTileWidth, 0);

    int ok = stbi_write_png(
        outputPath.string().c_str(),
        canvas.width, canvas.height, kChannels,
        canvas.pixels.data(),
        canvas.width * kChannels);

    if (!ok) {
        std::cerr << "Ошибка: не удалось сохранить PNG \"" << outputPath.string() << "\"\n";
        return 1;
    }

    std::cout << "Готово: " << outputPath.string() << "\n";
    return 0;
}
