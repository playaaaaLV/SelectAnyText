#pragma once

#include "screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stop_token>
#include <vector>

// Locate repeated foreground/outline colors without a font or language dictionary.
namespace caption {
struct Color {
    int r, g, b;
};
inline Color Rgb(uint32_t p) {
    return {int(p >> 16 & 255), int(p >> 8 & 255), int(p & 255)};
}
inline int Distance(Color a, Color b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}
inline int Bin(Color p) {
    return (p.r >> 5) * 64 + (p.g >> 5) * 8 + (p.b >> 5);
}
struct Line {
    RECT box;
    Color fill, edge;
    int strength;
};

inline std::vector<Line> Locate(const Image &image, std::stop_token cancel) {
    const int w = image.width, h = image.height;
    struct Sum {
        int r = 0, g = 0, b = 0, n = 0;
    };
    std::array<Sum, 512> palette{};
    std::vector<int> votes(512 * 512);
    for (int y = 0; y < h; ++y) {
        if (cancel.stop_requested()) return {};
        for (int x = 0; x < w; ++x) {
            const int pos = y * w + x;
            const auto p = Rgb(image.pixels[pos]);
            const int a = Bin(p);
            auto &sum = palette[a];
            sum.r += p.r;
            sum.g += p.g;
            sum.b += p.b;
            ++sum.n;
            for (int step : {x + 2 < w ? 2 : 0, y + 2 < h ? 2 * w : 0}) {
                const auto q = Rgb(image.pixels[pos + step]);
                if (Distance(p, q) < 90) continue;
                const int b = Bin(q);
                ++votes[std::min(a, b) * 512 + std::max(a, b)];
            }
        }
    }
    std::vector<Line> lines;
    const auto mean = [&](int bin) {
        const auto &s = palette[bin];
        return Color{s.r / s.n, s.g / s.n, s.b / s.n};
    };
    std::vector<std::pair<Color, Color>> pairs;
    for (int pair = 0; pair < 16 && pairs.size() < 7; ++pair) {
        auto peak = std::max_element(votes.begin(), votes.end());
        if (*peak < 24) break;
        const int key = int(peak - votes.begin());
        *peak = 0;
        Color fill = mean(key / 512), edge = mean(key % 512);
        const auto closePair = [&](const auto &p) {
            return (Distance(p.first, fill) < 48 && Distance(p.second, edge) < 48) ||
                   (Distance(p.second, fill) < 48 && Distance(p.first, edge) < 48);
        };
        if (std::none_of(pairs.begin(), pairs.end(), closePair)) pairs.emplace_back(fill, edge);
    }
    for (auto [fill, edge] : pairs) {
        for (int polarity = 0; polarity < 2; ++polarity, std::swap(fill, edge)) {
            std::vector<unsigned char> core(w * h);
            const int contrast = Distance(fill, edge);
            for (int y = 0; y < h; ++y) {
                if (cancel.stop_requested()) return {};
                for (int x = 0; x < w; ++x) {
                    if (Distance(Rgb(image.pixels[y * w + x]), fill) > std::clamp(contrast / 10, 14, 24))
                        continue;
                    int hits = 0;
                    for (int dy = -4; dy <= 4; dy += 2)
                        for (int dx = -4; dx <= 4; dx += 2) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx >= 0 && xx < w && yy >= 0 && yy < h &&
                                Distance(Rgb(image.pixels[yy * w + xx]), edge) < contrast * .45)
                                ++hits;
                        }
                    core[y * w + x] = hits >= 7;
                }
            }
            std::vector<int> stack, rows(h);
            for (int start = 0; start < w * h; ++start) {
                if (core[start] != 1) continue;
                stack.clear();
                stack.push_back(start);
                core[start] = 0;
                int top = start / w, bottom = top, left = start % w, right = left;
                for (size_t k = 0; k < stack.size(); ++k) {
                    if ((k % 8192) == 0 && cancel.stop_requested()) return {};
                    const int x = stack[k] % w, y = stack[k] / w;
                    left = std::min(left, x);
                    right = std::max(right, x);
                    bottom = std::max(bottom, y);
                    for (int yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1); ++yy)
                        for (int xx = std::max(0, x - 1); xx <= std::min(w - 1, x + 1); ++xx) {
                            const int p = yy * w + xx;
                            if (core[p] == 1) {
                                core[p] = 0;
                                stack.push_back(p);
                            }
                        }
                }
                if (bottom - top < 3 || right - left > 6 * (bottom - top + 1) || stack.size() < 7) continue;
                for (int p : stack) {
                    core[p] = 2;
                    ++rows[p / w];
                }
            }
            const int threshold = std::max(4, *std::max_element(rows.begin(), rows.end()) / 8);
            for (int y = 0; y < h;) {
                if (rows[y] < threshold) {
                    ++y;
                    continue;
                }
                int top = y, last = y;
                while (++y < h && y - last <= 2)
                    if (rows[y] >= threshold) last = y;
                const int height = last - top + 1;
                if (height < 6) continue;
                int left = -1, right = -1, weight = 0;
                for (int x = 0; x <= w; ++x) {
                    int count = 0;
                    if (x < w)
                        for (int yy = top; yy <= last; ++yy)
                            count += core[yy * w + x] == 2;
                    if (count >= 2) {
                        if (left < 0) left = x;
                        right = x;
                        weight += count;
                    }
                    if (left < 0 || (x < w && x - right < height)) continue;
                    if (right - left >= 2 * height && weight >= 3 * height) {
                        const int pad = std::clamp(height / 4, 3, 12);
                        lines.push_back({{std::max(0, left - pad), std::max(0, top - pad),
                                          std::min(w, right + 1 + pad), std::min(h, last + 1 + pad)},
                                         fill,
                                         edge,
                                         weight});
                    }
                    left = -1;
                    weight = 0;
                }
            }
        }
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const auto &a, const auto &b) { return a.strength > b.strength; });
    std::vector<Line> unique;
    for (const auto &line : lines) {
        bool duplicate = false;
        for (const auto &prev : unique) {
            RECT intersection{};
            if (IntersectRect(&intersection, &prev.box, &line.box) &&
                intersection.bottom - intersection.top > .65 * (line.box.bottom - line.box.top) &&
                intersection.right - intersection.left > .65 * (line.box.right - line.box.left) &&
                Distance(prev.fill, line.fill) < 45 && Distance(prev.edge, line.edge) < 60)
                duplicate = true;
        }
        if (!duplicate) unique.push_back(line);
        if (unique.size() == 8) break;
    }
    return unique;
}

struct Raster {
    int w, h, scale, padding;
    std::vector<unsigned char> pixels;
};
// A closed outline separates letter interiors from both the picture and the
// counters. Vote across enclosed components to identify the recurring fill.
inline std::vector<std::vector<unsigned char>> OutlineColors(const Image &image) {
    std::array<int, 512> counts{};
    for (auto p : image.pixels) {
        auto c = Rgb(p);
        if (std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}) > 45) ++counts[Bin(c)];
    }
    std::vector<std::vector<unsigned char>> masks;
    for (int i = 0; i < 3; ++i) {
        auto best = std::max_element(counts.begin(), counts.end());
        if (*best < 32) break;
        const int bin = int(best - counts.begin());
        Color center{32 * (bin / 64) + 16, 32 * ((bin / 8) % 8) + 16, 32 * (bin % 8) + 16};
        for (int j = 0; j < 512; ++j) {
            Color other{32 * (j / 64) + 16, 32 * ((j / 8) % 8) + 16, 32 * (j % 8) + 16};
            if (Distance(other, center) <= 32) counts[j] = 0;
        }
        masks.emplace_back(image.pixels.size());
        auto &mask = masks.back();
        for (size_t j = 0; j < mask.size(); ++j)
            mask[j] = Distance(Rgb(image.pixels[j]), center) < 64 ? 0 : 255;
    }
    return masks;
}
inline Raster EnclosedFill(const Image &image, std::vector<unsigned char> mask, std::stop_token cancel,
                           bool tight) {
    const int w = image.width, h = image.height;
    const auto walls = mask;
    std::vector<int> stack;
    std::array<double, 512> votes{};
    std::vector<unsigned char> interior(w * h);
    for (int start = 0; start < w * h; ++start) {
        if (mask[start] != 255) continue;
        stack.clear();
        stack.push_back(start);
        mask[start] = 1;
        bool outside = false;
        for (size_t i = 0; i < stack.size(); ++i) {
            if ((i % 8192) == 0 && cancel.stop_requested()) return {};
            const int p = stack[i], x = p % w, y = p / w;
            outside |= x == 0 || x == w - 1 || y == 0 || y == h - 1;
            for (int q :
                 {x > 0 ? p - 1 : p, x + 1 < w ? p + 1 : p, y > 0 ? p - w : p, y + 1 < h ? p + w : p}) {
                if (mask[q] == 255) {
                    mask[q] = 1;
                    stack.push_back(q);
                }
            }
        }
        if (outside || stack.size() < 4) continue;
        std::array<int, 512> counts{};
        for (int p : stack)
            ++counts[Bin(Rgb(image.pixels[p]))];
        const auto best = std::max_element(counts.begin(), counts.end());
        votes[best - counts.begin()] += std::sqrt(double(*best));
        for (int p : stack)
            interior[p] = 1;
    }
    const auto best = std::max_element(votes.begin(), votes.end());
    if (*best < 8) return {};
    const int bin = int(best - votes.begin());
    Color center{32 * (bin / 64) + 16, 32 * ((bin / 8) % 8) + 16, 32 * (bin % 8) + 16};
    if (tight) {
        std::array<int, 512> fine{};
        for (int i = 0; i < w * h; ++i)
            if (interior[i] && Bin(Rgb(image.pixels[i])) == bin) {
                const auto c = Rgb(image.pixels[i]);
                ++fine[((c.r & 31) / 4) * 64 + ((c.g & 31) / 4) * 8 + ((c.b & 31) / 4)];
            }
        const int j = int(std::max_element(fine.begin(), fine.end()) - fine.begin());
        center = {32 * (bin / 64) + 4 * (j / 64) + 2, 32 * ((bin / 8) % 8) + 4 * ((j / 8) % 8) + 2,
                  32 * (bin % 8) + 4 * (j % 8) + 2};
    }
    Raster r{w * 2 + 16, h * 2 + 16, 2, 8, {}};
    r.pixels.assign(r.w * r.h, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (x == 0 && cancel.stop_requested()) return {};
            const int p = y * w + x;
            const auto delta = Distance(Rgb(image.pixels[p]), center);
            bool glyph = tight ? delta < 13 : interior[p] && delta < 54;
            if (!tight && !glyph && delta <= 21) {
                // Allow letters cut by the selection edge or gaps in the outline.
                for (int dy = -5; dy <= 5 && !glyph; dy += 2)
                    for (int dx = -5; dx <= 5; dx += 2) {
                        if (walls[std::clamp(y + dy, 0, h - 1) * w + std::clamp(x + dx, 0, w - 1)] == 0) {
                            glyph = true;
                            break;
                        }
                    }
            }
            if (glyph) {
                const int q = (2 * y + 8) * r.w + 2 * x + 8;
                r.pixels[q] = r.pixels[q + 1] = r.pixels[q + r.w] = r.pixels[q + r.w + 1] = 0;
            }
        }
    return r;
}

inline Raster Render(const Image &image, const Line &line, double shear, std::stop_token cancel) {
    const int width = line.box.right - line.box.left, height = line.box.bottom - line.box.top;
    const int scale = std::clamp(80 / height, 2, 4), pad = 12 + height * scale / 6;
    Raster r{width * scale + 2 * pad, height * scale + 2 * pad, scale, pad, {}};
    if (size_t(r.w) * r.h > 2000000) return {};
    r.pixels.assign(r.w * r.h, 255);
    const int contrast = Distance(line.fill, line.edge);
    const bool colored = std::max({line.edge.r, line.edge.g, line.edge.b}) -
                                 std::min({line.edge.r, line.edge.g, line.edge.b}) >
                             55 ||
                         std::max({line.fill.r, line.fill.g, line.fill.b}) -
                                 std::min({line.fill.r, line.fill.g, line.fill.b}) >
                             55;
    std::vector<unsigned char> boundary(r.w * r.h), ink(r.w * r.h);
    for (int y = 0; y < height * scale; ++y)
        for (int x = 0; x < width * scale; ++x) {
            if (x == 0 && cancel.stop_requested()) return {};
            const double sx = std::clamp(line.box.left + (x + .5) / scale - .5, 0., double(image.width - 1));
            const double sy = std::clamp(line.box.top + (y + .5) / scale - .5, 0., double(image.height - 1));
            const int xx = int(sx), yy = int(sy);
            const auto at = [&](int dx, int dy) {
                return Rgb(image.pixels[std::min(image.height - 1, yy + dy) * image.width +
                                        std::min(image.width - 1, xx + dx)]);
            };
            const auto a = at(0, 0), b = at(1, 0), c = at(0, 1), d = at(1, 1);
            const double u = sx - xx, v = sy - yy;
            const auto lerp = [&](int Color::*channel) {
                return int((1 - v) * ((1 - u) * (a.*channel) + u * (b.*channel)) +
                           v * ((1 - u) * (c.*channel) + u * (d.*channel)));
            };
            const Color p{lerp(&Color::r), lerp(&Color::g), lerp(&Color::b)};
            const int pos = (y + pad) * r.w + x + pad;
            boundary[pos] = Distance(p, line.edge) < contrast * .43;
            ink[pos] = Distance(p, line.fill) < (colored ? std::clamp(contrast / 9, 16, 30) : contrast * .23);
            if (colored && ink[pos]) {
                bool nearEdge = false;
                const int radius = std::clamp(height / 5, 3, 9);
                for (int dy = -radius; dy <= radius && !nearEdge; dy += 2)
                    for (int dx = -radius; dx <= radius; dx += 2) {
                        const auto neighbor =
                            image.pixels[std::clamp(yy + dy, 0, image.height - 1) * image.width +
                                         std::clamp(xx + dx, 0, image.width - 1)];
                        if (Distance(Rgb(neighbor), line.edge) < contrast * .45) {
                            nearEdge = true;
                            break;
                        }
                    }
                ink[pos] = nearEdge;
            }
        }
    if (!colored) {
        // Small illustration strokes can enclose holes too. Keep outline
        // components whose vertical extent is comparable to the caption row.
        std::vector<int> component;
        for (int start = 0; start < r.w * r.h; ++start) {
            if (boundary[start] != 1) continue;
            component.clear();
            component.push_back(start);
            boundary[start] = 3;
            int bottom = start / r.w, left = start % r.w, right = left;
            for (size_t i = 0; i < component.size(); ++i) {
                int p = component[i], x = p % r.w, y = p / r.w;
                left = std::min(left, x);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int q = p + dy * r.w + dx;
                        if (boundary[q] == 1) {
                            boundary[q] = 3;
                            component.push_back(q);
                        }
                    }
            }
            const int bh = bottom - start / r.w + 1;
            if (bh < height * scale / 3 || component.size() < .15 * bh * (right - left + 1))
                for (int p : component)
                    boundary[p] = 0;
        }
    }
    // Flood the exterior so that similarly colored illustration details stay out.
    std::vector<int> queue{0};
    boundary[0] = 2;
    for (size_t i = 0; i < queue.size(); ++i) {
        if ((i % 8192) == 0 && cancel.stop_requested()) return {};
        const int p = queue[i], x = p % r.w;
        for (int q : {x > 0 ? p - 1 : p, x + 1 < r.w ? p + 1 : p, p >= r.w ? p - r.w : p,
                      p + r.w < r.w * r.h ? p + r.w : p}) {
            if (!boundary[q]) {
                boundary[q] = 2;
                queue.push_back(q);
            }
        }
    }
    for (int y = pad; y < r.h - pad; ++y)
        for (int x = pad; x < r.w - pad; ++x) {
            const int p = y * r.w + x;
            if ((colored || !boundary[p]) && ink[p])
                r.pixels[p + int(std::lround(shear * (y - r.h * .5)))] = 0;
        }
    return r;
}
} // namespace caption
