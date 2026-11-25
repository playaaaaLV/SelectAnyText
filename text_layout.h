#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <vector>

struct TextStop {
    float x;
    size_t offset;
};

struct TextWord {
    std::vector<TextStop> stops;
    RECT bounds{};
};

struct TextLine {
    RECT bounds{};
    std::vector<TextWord> words;
};

struct TextFragment {
    TextLine line;
    std::wstring text;
};

struct TextSpan {
    POINT first;
    POINT last;
    size_t offset;
    size_t length;
};

struct RegionSelection {
    std::wstring text;
    std::vector<TextSpan> spans;
    std::vector<RECT> highlights;
    std::vector<std::array<POINT, 4>> polygons; // Only for oblique bands, in highlight order.
};

// Screen position of the reading raster's origin and unit baseline vector.
struct ReadingFrame {
    double x = 0, y = 0, cosine = 1, sine = 0;
    POINT OnScreen(double u, double v) const;
};

struct TextLayout {
    std::wstring text;
    std::vector<TextLine> lines;

    ReadingFrame frame;
    static TextLayout FromFragments(std::vector<TextFragment> fragments, ReadingFrame frame = {});
    RegionSelection Select(POINT anchor, POINT cursor) const;
};
