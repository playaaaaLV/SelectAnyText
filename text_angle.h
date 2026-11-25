#pragma once

#include "screen.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stop_token>

namespace text_angle {
constexpr double radians = std::numbers::pi / 180;

inline int Luma(uint32_t p) {
    return (77 * ((p >> 16) & 255) + 150 * ((p >> 8) & 255) + 29 * (p & 255)) >> 8;
}

// Project contrast edges onto candidate line normals. Edges belonging to
// multiple letters reinforce each other at their shared baseline and cap line.
inline double Estimate(const Image& image, std::stop_token cancel) {
    const int w = image.width, h = image.height;
    if (w < 8 || h < 8) return 0;
    const int step = std::max(1, (std::max(w, h) + 899) / 900);
    struct Edge { float x, y, nx, ny; };
    std::vector<Edge> edges;
    for (int y = step; y < h - step; y += step) {
        if (cancel.stop_requested()) return 0;
        for (int x = step; x < w - step; x += step) {
            const auto* p = image.pixels.data() + size_t(y) * w + x;
            const float dx = float(Luma(p[step]) - Luma(p[-step]));
            const float dy = float(Luma(p[w * step]) - Luma(p[-w * step]));
            if (dx * dx + dy * dy < 32 * 32) continue;
            const float length = std::sqrt(dx * dx + dy * dy);
            edges.push_back({float(x / step), float(y / step), dx / length, dy / length});
        }
    }
    if (edges.size() < 48) return 0;
    const size_t stride = (edges.size() + 11999) / 12000;
    const int radius = int(std::ceil(std::hypot(double(w), double(h)) / step)) + 2;
    std::vector<float> bins(2 * radius + 1);
    const auto score = [&](double angle) {
        const float nx = float(-std::sin(angle * radians)), ny = float(std::cos(angle * radians));
        std::fill(bins.begin(), bins.end(), 0.f);
        for (size_t i = 0; i < edges.size(); i += stride) {
            const auto& e = edges[i];
            const float dot = nx * e.nx + ny * e.ny;
            const float weight = dot * dot - .15f;
            if (weight > 0) {
                const float pos = radius + nx * e.x + ny * e.y;
                const int bin = int(pos);
                const float fraction = pos - bin;
                bins[bin] += weight * (1 - fraction);
                bins[bin + 1] += weight * fraction;
            }
        }
        double sum = 0;
        for (const float b : bins) sum += b * b;
        return sum;
    };
    double best = 0, angle = 0, total = 0;
    for (int a = -90; a < 90; a += 3) {
        if (cancel.stop_requested()) return 0;
        const double merit = score(a);
        total += merit;
        if (merit > best) { best = merit; angle = a; }
    }
    if (best < 1.45 * total / 60) return 0;
    const double coarse = angle;
    for (int i = -11; i <= 11; ++i) {
        const double a = coarse + i * .25;
        const double merit = score(a);
        if (merit > best) { best = merit; angle = a; }
    }
    return std::remainder(angle, 180.);
}

struct Raster {
    Image image;
    ReadingFrame frame;
};

// Pixel edges determine the inverse transform used by the highlight. Sample
// pixel centers for OCR; quarter turns consequently preserve pixels exactly.
inline Raster Align(const Image& source, double degrees, std::stop_token cancel) {
    const auto snap = [](double value) {
        return std::abs(value - std::round(value)) < 1e-10 ? std::round(value) : value;
    };
    const double c = snap(std::cos(degrees * radians)), s = snap(std::sin(degrees * radians));
    const int sw = source.width, sh = source.height;
    const int w = int(std::ceil(std::abs(c) * sw + std::abs(s) * sh));
    const int h = int(std::ceil(std::abs(s) * sw + std::abs(c) * sh));
    Raster result;
    // Avoid allocating a huge diagonal square for a long, narrow crop.
    if (size_t(w) * h > std::max(size_t(16000000), source.pixels.size() * 3)) return result;
    auto& [image, frame] = result;
    image.width = w;
    image.height = h;
    const double ox = (sw - c * w + s * h) * .5, oy = (sh - s * w - c * h) * .5;
    frame = {source.origin.x + ox, source.origin.y + oy, c, s};
    std::array corners{source.pixels.front(), source.pixels[sw - 1],
                       source.pixels[source.pixels.size() - sw], source.pixels.back()};
    std::sort(corners.begin(), corners.end(), [](auto a, auto b) { return Luma(a) < Luma(b); });
    image.pixels.resize(size_t(w) * h, corners[1]);
    for (int y = 0; y < h; ++y) {
        if (cancel.stop_requested()) return {};
        double sx = ox + c * .5 - s * (y + .5) - .5;
        double sy = oy + s * .5 + c * (y + .5) - .5;
        for (int x = 0; x < w; ++x, sx += c, sy += s) {
            if (sx < -.5 || sy < -.5 || sx >= sw - .5 || sy >= sh - .5) continue;
            const double px = std::clamp(sx, 0., double(sw - 1)), py = std::clamp(sy, 0., double(sh - 1));
            const int ix = int(px), iy = int(py);
            const auto* row = source.pixels.data() + size_t(iy) * sw;
            auto& pixel = image.pixels[size_t(y) * w + x];
            if (c == 0 || s == 0) { pixel = row[ix]; continue; }
            const auto* next = iy + 1 < sh ? row + sw : row;
            const int jx = std::min(ix + 1, sw - 1);
            pixel = 0;
            for (int shift : {0, 8, 16}) {
                const auto channel = [shift](uint32_t p) { return double((p >> shift) & 255); };
                const double a = std::lerp(channel(row[ix]), channel(row[jx]), px - ix);
                const double b = std::lerp(channel(next[ix]), channel(next[jx]), px - ix);
                pixel |= uint32_t(std::lround(std::lerp(a, b, py - iy))) << shift;
            }
        }
    }
    return result;
}
}
