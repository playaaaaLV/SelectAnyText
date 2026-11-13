#pragma once

#include <windows.h>

#include <memory>
#include <stop_token>
#include <string>
#include <vector>

struct Image;

struct RecognizedWord {
    std::wstring text;
    RECT bounds;
};

class MultilingualOcr {
public:
    MultilingualOcr();
    ~MultilingualOcr();

    explicit operator bool() const;
    std::vector<std::vector<RecognizedWord>> Recognize(const Image& image, std::stop_token cancel = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
