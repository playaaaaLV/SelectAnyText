#pragma once

#include <windows.h>

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
};

struct TextLayout {
    std::wstring text;
    std::vector<TextLine> lines;

    static TextLayout FromFragments(std::vector<TextFragment> fragments);
    RegionSelection Select(POINT anchor, POINT cursor) const;
};
