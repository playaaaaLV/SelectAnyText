#pragma once

#include "multilingual_ocr.h"
#include <string_view>

size_t OcrLetterCount(std::wstring_view text);
std::vector<std::vector<unsigned char>> ColorTextMasks(const Image& image, std::stop_token cancel);
std::vector<std::vector<RecognizedWord>> ReliableColorText(std::vector<std::vector<RecognizedWord>> lines);
double ColorTextScore(const std::vector<std::vector<RecognizedWord>>& lines);
std::vector<std::vector<RecognizedWord>> MergeColorText(
    std::vector<std::vector<RecognizedWord>> original,
    const std::vector<std::vector<RecognizedWord>>& colored);
