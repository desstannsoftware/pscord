// ps5-discord - PNG, JPEG and GIF decoding with stb_image, plus a box filter.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT

#include "dc/media.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

namespace dc {

namespace {

// A GIF holds every frame at full size while it decodes; past this it is shown still.
constexpr size_t kMaxGifBytes = 160u * 1024 * 1024;
// What the frames may take once shrunk.
constexpr size_t kMaxFrameBytes = 48u * 1024 * 1024;

// Area-averaging shrink; sw x sh -> dw x dh, both RGBA.
void shrink(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    const float fx = float(sw) / float(dw);
    const float fy = float(sh) / float(dh);
    for (int y = 0; y < dh; ++y) {
        const int y0 = int(float(y) * fy);
        const int y1 = std::max(y0 + 1, std::min(sh, int(float(y + 1) * fy)));
        for (int x = 0; x < dw; ++x) {
            const int x0 = int(float(x) * fx);
            const int x1 = std::max(x0 + 1, std::min(sw, int(float(x + 1) * fx)));
            unsigned sum[4] = {0, 0, 0, 0};
            unsigned weight = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* p = src + (size_t(yy) * size_t(sw) + size_t(x0)) * 4;
                for (int xx = x0; xx < x1; ++xx, p += 4) {
                    // Weight colour by alpha so transparent edges do not darken.
                    const unsigned a = p[3];
                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    sum[3] += a;
                    ++weight;
                }
            }
            uint8_t* out = dst + (size_t(y) * size_t(dw) + size_t(x)) * 4;
            if (sum[3] == 0) {
                out[0] = out[1] = out[2] = out[3] = 0;
            } else {
                out[0] = uint8_t(sum[0] / sum[3]);
                out[1] = uint8_t(sum[1] / sum[3]);
                out[2] = uint8_t(sum[2] / sum[3]);
                out[3] = uint8_t(sum[3] / weight);
            }
        }
    }
}

void fit(int w, int h, int max_w, int max_h, int& out_w, int& out_h) {
    const float scale = std::min({1.0f, float(max_w) / float(w), float(max_h) / float(h)});
    out_w = std::max(1, int(std::lround(float(w) * scale)));
    out_h = std::max(1, int(std::lround(float(h) * scale)));
}

std::vector<uint8_t> resized(const uint8_t* src, int w, int h, int dw, int dh) {
    std::vector<uint8_t> frame(size_t(dw) * size_t(dh) * 4);
    if (dw == w && dh == h) {
        std::memcpy(frame.data(), src, frame.size());
    } else {
        shrink(src, w, h, frame.data(), dw, dh);
    }
    return frame;
}

} // namespace

bool decode_picture(const std::string& bytes, int max_w, int max_h, Picture& out) {
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len = int(bytes.size());
    int w = 0;
    int h = 0;
    int comp = 0;
    if (bytes.size() < 8 || !stbi_info_from_memory(data, len, &w, &h, &comp) || w <= 0 || h <= 0) {
        return false;
    }
    int dw = 0;
    int dh = 0;
    fit(w, h, max_w, max_h, dw, dh);
    out.width = dw;
    out.height = dh;

    const bool gif = bytes.compare(0, 4, "GIF8") == 0;
    if (gif) {
        int* delays = nullptr;
        int frames = 0;
        // Only worth the full decode when every frame fits in memory.
        stbi_uc* all = nullptr;
        if (size_t(w) * size_t(h) * 4 * 64 <= kMaxGifBytes || bytes.size() < 4u * 1024 * 1024) {
            all = stbi_load_gif_from_memory(data, len, &delays, &w, &h, &frames, &comp, 4);
        }
        if (all && frames > 0 && size_t(w) * size_t(h) * 4 * size_t(frames) <= kMaxGifBytes) {
            const size_t frame_bytes = size_t(dw) * size_t(dh) * 4;
            const int keep = int(std::min<size_t>(size_t(frames), std::max<size_t>(1, kMaxFrameBytes / frame_bytes)));
            const int step = (frames + keep - 1) / keep;
            for (int i = 0; i < frames; i += step) {
                out.frames.push_back(resized(all + size_t(w) * size_t(h) * 4 * size_t(i), w, h, dw, dh));
                int delay = 0;
                for (int j = i; j < std::min(frames, i + step); ++j) {
                    delay += delays && delays[j] > 0 ? delays[j] : 100;
                }
                out.delays_ms.push_back(std::max(20, delay));
            }
            stbi_image_free(all);
            std::free(delays);
            if (out.frames.size() == 1) {
                out.delays_ms.clear();
            }
            return true;
        }
        stbi_image_free(all);
        std::free(delays);
    }
    stbi_uc* pixels = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!pixels) {
        return false;
    }
    out.frames.push_back(resized(pixels, w, h, dw, dh));
    stbi_image_free(pixels);
    return true;
}

} // namespace dc
