#include "text_layout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr LONG highlightPadding = 2;

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

RECT ReadingBounds(const TextLine& line) {
    std::vector<LONG> heights;
    for (const auto& word : line.words) {
        if (word.stops.size() > 2 && word.bounds.bottom > word.bounds.top) {
            heights.push_back(word.bounds.bottom - word.bounds.top);
        }
    }
    if (heights.size() < 2) return line.bounds;
    std::sort(heights.begin(), heights.end());
    const auto typical = heights[(heights.size() - 1) / 2];
    RECT result = line.bounds;
    bool first = true;
    for (const auto& word : line.words) {
        const auto height = word.bounds.bottom - word.bounds.top;
        // One oversized OCR box must not stretch a whole row onto the next row.
        // Keep normal ascender/descender differences and small punctuation.
        if (height * 5 > typical * 8) continue;
        result.top = first ? word.bounds.top : std::min(result.top, word.bounds.top);
        result.bottom = first ? word.bounds.bottom : std::max(result.bottom, word.bounds.bottom);
        first = false;
    }
    return result;
}

void SeparateBands(std::vector<RECT>& bands) {
    for (size_t i = 0; i < bands.size(); ++i) {
        for (size_t j = i + 1; j < bands.size(); ++j) {
            auto* upper = &bands[i];
            auto* lower = &bands[j];
            if (CenterY(*upper) > CenterY(*lower)) std::swap(upper, lower);
            if (CenterY(*upper) == CenterY(*lower) || upper->bottom <= lower->top ||
                upper->right <= lower->left || lower->right <= upper->left) continue;
            // Share a mistaken overlap rather than painting it twice. Only the
            // display bands change; word positions and clipboard text stay intact.
            const auto boundary = std::clamp((upper->bottom + lower->top) / 2,
                upper->top + 1, std::max(upper->top + 1, lower->bottom - 2));
            upper->bottom = boundary;
            lower->top = boundary + 1;
        }
    }
}

}

TextLayout TextLayout::FromFragments(std::vector<TextFragment> fragments, ReadingFrame frame) {
    std::vector<RECT> originalBounds;
    for (const auto& fragment : fragments) originalBounds.push_back(fragment.line.bounds);
    for (size_t i = 0; i < fragments.size(); ++i) {
        const auto original = originalBounds[i];
        const auto reading = ReadingBounds(fragments[i].line);
        // Mixed font sizes are legitimate. Correct an outlier only when it
        // reaches into another row that the normal words do not touch.
        for (size_t j = 0; j < originalBounds.size(); ++j) {
            if (i == j) continue;
            const auto other = originalBounds[j];
            RECT overlap{};
            if (IntersectRect(&overlap, &original, &other) &&
                (reading.bottom <= other.top || reading.top >= other.bottom)) {
                fragments[i].line.bounds = reading;
                break;
            }
        }
    }
    std::stable_sort(fragments.begin(), fragments.end(), [](const auto& first, const auto& second) {
        return CenterY(first.line.bounds) < CenterY(second.line.bounds);
    });

    TextLayout layout;
    layout.frame = frame;
    auto first = fragments.begin();
    while (first != fragments.end()) {
        auto last = first + 1;
        while (last != fragments.end() && SameRow(first->line.bounds, last->line.bounds)) {
            const auto a = first->line.bounds.bottom - first->line.bounds.top;
            const auto b = last->line.bounds.bottom - last->line.bounds.top;
            if (frame.sine != 0 && std::max(a,b) > 2 * std::min(a,b)) break;
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

POINT ReadingFrame::OnScreen(double u, double v) const {
    return {std::lround(x + cosine * u - sine * v), std::lround(y + sine * u + cosine * v)};
}

RegionSelection TextLayout::Select(POINT anchor, POINT cursor) const {
    RegionSelection result;
    if (anchor.x == cursor.x && anchor.y == cursor.y) {
        return result;
    }
    // Test intervals on both screen axes and both reading axes. This also
    // excludes letters in the two empty corners of an inclined band's bounds.
    const double cx = (double(anchor.x) + cursor.x) / 2 - frame.x;
    const double cy = (double(anchor.y) + cursor.y) / 2 - frame.y;
    const double hw = std::abs(double(anchor.x) - cursor.x) / 2;
    const double hh = std::abs(double(anchor.y) - cursor.y) / 2;
    const double c = frame.cosine, s = frame.sine, ac = std::abs(c), as = std::abs(s);
    const double u = c * cx + s * cy, v = -s * cx + c * cy;
    const double rw = ac * hw + as * hh, rh = as * hw + ac * hh;
    const bool oblique = c != 0 && s != 0;

    for (const auto& line : lines) {
        if (line.bounds.bottom <= v - rh || line.bounds.top > v + rh ||
            line.bounds.right <= u - rw || line.bounds.left >= u + rw) {
            continue;
        }
        bool firstInLine = true;
        bool adjacent = false;
        size_t previousEnd = 0;
        const auto height = line.bounds.bottom - line.bounds.top;
        // OCR can omit the dark outline below colored glyphs. Scale the margin
        // with the text, then let SeparateBands constrain it between close rows.
        const auto bottomPadding = std::max(highlightPadding, (height + 9) / 10);
        for (const auto& word : line.words) {
            const auto& stops = word.stops;
            const auto y = CenterY(word.bounds) / 2;
            const auto contains = [&](size_t index) {
                const auto left = std::min(stops[index].x, stops[index + 1].x);
                const auto right = std::max(stops[index].x, stops[index + 1].x);
                if (right <= u - rw || left >= u + rw ||
                    word.bounds.bottom <= v - rh || word.bounds.top > v + rh) return false;
                if (!oblique) return true;
                const double du = (left + right) / 2 - u;
                const double dv = (word.bounds.top + word.bounds.bottom) * .5 - v;
                const double gw = (right - left) * .5, gh = (word.bounds.bottom - word.bounds.top) * .5;
                return std::abs(c * du - s * dv) < hw + ac * gw + as * gh &&
                       std::abs(s * du + c * dv) < hh + as * gw + ac * gh;
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
                const RECT rect{static_cast<LONG>(std::floor(std::min(start.x, end.x))),
                    line.bounds.top - highlightPadding,
                    static_cast<LONG>(std::ceil(std::max(start.x, end.x))),
                    line.bounds.bottom + bottomPadding};

                bool merge = false;
                if (firstInLine) {
                    if (!result.text.empty()) {
                        result.text += L"\r\n";
                    }
                } else if (adjacent) {
                    const auto& previous = result.highlights.back();
                    const auto gap = std::max(previous.left, rect.left) - std::min(previous.right, rect.right);
                    // Match PDFchic's continuous reading bands, retaining large column gaps.
                    merge = gap <= 6 * height;
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
    for (auto& highlight : result.highlights) {
        highlight.left -= highlightPadding;
        highlight.right += highlightPadding;
    }
    SeparateBands(result.highlights);
    if (frame.x != 0 || frame.y != 0 || c != 1 || s != 0) {
        for (auto& box : result.highlights) {
            const std::array corners{frame.OnScreen(box.left, box.top), frame.OnScreen(box.right, box.top),
                frame.OnScreen(box.right, box.bottom), frame.OnScreen(box.left, box.bottom)};
            if (oblique) result.polygons.push_back(corners);
            box = {corners[0].x, corners[0].y, corners[0].x, corners[0].y};
            for (const auto p : corners) {
                box.left = std::min(box.left, p.x);
                box.top = std::min(box.top, p.y);
                box.right = std::max(box.right, p.x);
                box.bottom = std::max(box.bottom, p.y);
            }
        }
        for (auto& span : result.spans) {
            span.first = frame.OnScreen(span.first.x, span.first.y);
            span.last = frame.OnScreen(span.last.x, span.last.y);
        }
    }
    return result;
}
