#include "text_layout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
LONG CenterY(const RECT& bounds) {
    return bounds.top + bounds.bottom;
}

bool SameRow(const RECT& first, const RECT& second) {
    const auto firstHeight = first.bottom - first.top;
    const auto secondHeight = second.bottom - second.top;
    const auto overlap = std::min(first.bottom, second.bottom) - std::max(first.top, second.top);
    return 2 * overlap >= std::min(firstHeight, secondHeight) &&
        std::abs(CenterY(first) - CenterY(second)) <= std::max(firstHeight, secondHeight);
}

}

TextLayout TextLayout::FromFragments(std::vector<TextFragment> fragments) {
    std::stable_sort(fragments.begin(), fragments.end(), [](const auto& first, const auto& second) {
        return CenterY(first.line.bounds) < CenterY(second.line.bounds);
    });

    TextLayout layout;
    auto first = fragments.begin();
    while (first != fragments.end()) {
        auto last = first + 1;
        while (last != fragments.end() && SameRow(first->line.bounds, last->line.bounds)) {
            ++last;
        }
        const auto rtlCount = std::count_if(first, last, [](const auto& fragment) {
            const auto& stops = fragment.line.words.front().stops;
            return stops.front().x > stops.back().x;
        });
        const bool rtl = rtlCount > (last - first) / 2;
        std::stable_sort(first, last, [rtl](const auto& a, const auto& b) {
            return rtl ? a.line.bounds.right > b.line.bounds.right
                       : a.line.bounds.left < b.line.bounds.left;
        });

        if (!layout.lines.empty()) {
            layout.text += L"\r\n";
        }
        TextLine line;
        for (auto fragment = first; fragment != last; ++fragment) {
            if (fragment != first) {
                layout.text += L' ';
            }
            const auto offset = layout.text.size();
            layout.text += fragment->text;
            UnionRect(&line.bounds, &line.bounds, &fragment->line.bounds);
            for (auto& word : fragment->line.words) {
                for (auto& stop : word.stops) {
                    stop.offset += offset;
                }
                line.words.push_back(std::move(word));
            }
        }
        layout.lines.push_back(std::move(line));
        first = last;
    }
    return layout;
}

RegionSelection TextLayout::Select(POINT anchor, POINT cursor) const {
    RegionSelection result;
    if (anchor.x == cursor.x && anchor.y == cursor.y) {
        return result;
    }
    const RECT region{std::min(anchor.x, cursor.x), std::min(anchor.y, cursor.y),
        std::max(anchor.x, cursor.x), std::max(anchor.y, cursor.y)};

    for (const auto& line : lines) {
        if (line.bounds.bottom <= region.top || line.bounds.top > region.bottom ||
            line.bounds.right <= region.left || line.bounds.left >= region.right) {
            continue;
        }
        bool firstInLine = true;
        bool adjacent = false;
        size_t previousEnd = 0;
        for (const auto& word : line.words) {
            const auto& stops = word.stops;
            const auto y = CenterY(word.bounds) / 2;
            const auto height = word.bounds.bottom - word.bounds.top;
            const auto contains = [&](size_t index) {
                const auto left = std::min(stops[index].x, stops[index + 1].x);
                const auto right = std::max(stops[index].x, stops[index + 1].x);
                return right > region.left && left < region.right &&
                    word.bounds.bottom > region.top && word.bounds.top <= region.bottom;
            };
            for (size_t index = 0; index + 1 < stops.size();) {
                if (!contains(index)) {
                    adjacent = false;
                    ++index;
                    continue;
                }
                const auto first = index++;
                while (index + 1 < stops.size() && contains(index)) {
                    ++index;
                }
                const auto& start = stops[first];
                const auto& end = stops[index];
                const POINT firstPoint{std::lround(start.x), y};
                const POINT lastPoint{std::lround(end.x), y};
                const RECT rect{static_cast<LONG>(std::floor(std::min(start.x, end.x))), word.bounds.top - 1,
                    static_cast<LONG>(std::ceil(std::max(start.x, end.x))), word.bounds.bottom + 1};

                bool merge = false;
                if (firstInLine) {
                    if (!result.text.empty()) {
                        result.text += L"\r\n";
                    }
                } else if (adjacent) {
                    const auto& previous = result.highlights.back();
                    const auto gap = std::max(previous.left, rect.left) - std::min(previous.right, rect.right);
                    merge = gap <= height;
                    result.text.append(text, previousEnd, start.offset - previousEnd);
                } else {
                    result.text += L' ';
                }
                const auto offset = result.text.size();
                result.text.append(text, start.offset, end.offset - start.offset);
                if (merge) {
                    auto& span = result.spans.back();
                    span.last = lastPoint;
                    span.length = result.text.size() - span.offset;
                    auto& previous = result.highlights.back();
                    UnionRect(&previous, &previous, &rect);
                } else {
                    result.spans.push_back({firstPoint, lastPoint, offset, result.text.size() - offset});
                    result.highlights.push_back(rect);
                }
                previousEnd = end.offset;
                adjacent = true;
                firstInLine = false;
            }
        }
    }
    return result;
}
