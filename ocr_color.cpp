#include "ocr_color.h"
#include "screen.h"

#include <algorithm>
#include <array>

namespace {
// HSV hue on a circular 0..255 scale. Gray/dark pixels have no eligible hue.
int TextHue(uint32_t pixel) {
    const int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
    const int high = std::max({r, g, b}), low = std::min({r, g, b}), delta = high - low;
    if (high <= 150 || delta * 255 <= high * 140) return -1;
    double hue = high == r ? double(g - b) / delta : high == g ? 2.0 + double(b - r) / delta
                                                                      : 4.0 + double(r - g) / delta;
    if (hue < 0) hue += 6;
    return std::min(255, static_cast<int>(hue * 255 / 6));
}

}

size_t OcrLetterCount(std::wstring_view text) {
    size_t count = 0;
    for (const auto character : text) {
        WORD type = 0;
        GetStringTypeW(CT_CTYPE1, &character, 1, &type);
        count += (type & (C1_ALPHA | C1_DIGIT)) != 0;
    }
    return count;
}

namespace {
double Confidence(const std::vector<RecognizedWord>& line) {
    double weighted = 0, count = 0;
    for (const auto& word : line) {
        const auto length = std::max(size_t{1}, OcrLetterCount(word.text));
        weighted += word.confidence * length;
        count += length;
    }
    return count ? weighted / count : 0;
}

RECT Bounds(const std::vector<RecognizedWord>& line) {
    RECT bounds{};
    for (const auto& word : line) UnionRect(&bounds, &bounds, &word.bounds);
    return bounds;
}

bool Overlaps(RECT a, RECT b) {
    RECT intersection{};
    return IntersectRect(&intersection, &a, &b) &&
        2 * (intersection.bottom - intersection.top) >= std::min(a.bottom - a.top, b.bottom - b.top);
}
}

std::vector<std::vector<unsigned char>> ColorTextMasks(const Image& image, std::stop_token cancel) {
    std::array<size_t, 32> histogram{};
    std::vector<int16_t> hues(image.pixels.size());
    for (size_t i = 0; i < hues.size(); ++i) {
        if ((i & 16383) == 0 && cancel.stop_requested()) return {};
        const auto hue = TextHue(image.pixels[i]);
        hues[i] = static_cast<int16_t>(hue);
        if (hue >= 0) ++histogram[hue / 8];
    }
    std::vector<std::vector<unsigned char>> masks;
    // Two separated peaks keep the retry bounded; ordinary monochrome UI has none.
    for (int pass = 0; pass < 2; ++pass) {
        const auto peak = std::max_element(histogram.begin(), histogram.end());
        if (*peak < std::max(size_t{48}, image.pixels.size() / 1000)) break;
        const int bin = static_cast<int>(peak - histogram.begin()), center = bin * 8 + 4;
        std::vector<unsigned char> mask(hues.size(), 255);
        size_t ink = 0;
        for (size_t i = 0; i < hues.size(); ++i) {
            if ((i & 16383) == 0 && cancel.stop_requested()) return {};
            if (hues[i] < 0) continue;
            const auto difference = std::abs(hues[i] - center);
            if (std::min(difference, 256 - difference) < 10) { mask[i] = 0; ++ink; }
        }
        for (int offset = -2; offset <= 2; ++offset) histogram[(bin + offset + 32) % 32] = 0;
        // A solid colored panel is a background, not isolated foreground text.
        if (ink < image.pixels.size() / 2) masks.push_back(std::move(mask));
    }
    return masks;
}

std::vector<std::vector<RecognizedWord>> ReliableColorText(std::vector<std::vector<RecognizedWord>> lines) {
    for (auto& line : lines) {
        std::erase_if(line, [](const auto& word) { return word.confidence < 50 || OcrLetterCount(word.text) < 2; });
    }
    std::erase_if(lines, [](const auto& line) { return line.empty() || Confidence(line) < 60; });
    return lines;
}

double ColorTextScore(const std::vector<std::vector<RecognizedWord>>& lines) {
    size_t letters = 0;
    double score = 0;
    for (const auto& line : lines) for (const auto& word : line) {
        const auto count = OcrLetterCount(word.text);
        letters += count;
        score += count * word.confidence;
    }
    return letters >= 8 && score >= letters * 75 ? score : 0;
}

std::vector<std::vector<RecognizedWord>> MergeColorText(
    std::vector<std::vector<RecognizedWord>> original,
    const std::vector<std::vector<RecognizedWord>>& colored) {
    for (const auto& line : colored) {
        const auto box = Bounds(line);
        const auto confidence = Confidence(line);
        const bool keepOriginal = std::any_of(original.begin(), original.end(), [&](const auto& existing) {
            const auto oldBox = Bounds(existing);
            if (!Overlaps(oldBox, box)) return false;
            size_t oldLetters = 0, newLetters = 0;
            for (const auto& word : existing) oldLetters += OcrLetterCount(word.text);
            for (const auto& word : line) newLetters += OcrLetterCount(word.text);
            // A confident suffix alone must not block a substantially more complete word/line.
            const bool moreComplete = confidence >= 75 && newLetters * 2 > oldLetters * 3 &&
                (box.right - box.left) * 3 > (oldBox.right - oldBox.left) * 4;
            return !moreComplete && Confidence(existing) + 10 >= confidence;
        });
        if (keepOriginal) continue;
        for (auto& existing : original) {
            // Remove the entire unreliable line, including photo details read as letters.
            if (Overlaps(Bounds(existing), box)) existing.clear();
        }
        original.push_back(line);
    }
    std::erase_if(original, [](const auto& line) { return line.empty(); });
    return original;
}
