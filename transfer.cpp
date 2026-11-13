#include "transfer.h"
#include "multilingual_ocr.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace {
class WordMetrics {
public:
    WordMetrics() {
        font_ = CreateFontW(-32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        if (dc_ && font_) {
            oldFont_ = SelectObject(dc_, font_);
        }
    }

    ~WordMetrics() {
        if (oldFont_ && oldFont_ != HGDI_ERROR) {
            SelectObject(dc_, oldFont_);
        }
        if (font_) {
            DeleteObject(font_);
        }
        if (dc_) {
            DeleteDC(dc_);
        }
    }

    std::vector<TextStop> Stops(std::wstring_view text, const RECT& rect, size_t offset) const {
        const auto count = static_cast<int>(text.size());
        std::vector<int> advances(text.size());
        std::vector<WORD> types(text.size());
        SIZE size{};
        const bool measured = dc_ && GetTextExtentExPointW(dc_, text.data(), count,
            std::numeric_limits<int>::max(), nullptr, advances.data(), &size) && size.cx > 0;
        bool rightToLeft = false;
        if (GetStringTypeW(CT_CTYPE2, text.data(), count, types.data())) {
            for (const auto type : types) {
                if (type == C2_LEFTTORIGHT || type == C2_RIGHTTOLEFT) {
                    rightToLeft = type == C2_RIGHTTOLEFT;
                    break;
                }
            }
        }
        GetStringTypeW(CT_CTYPE3, text.data(), count, types.data());

        const auto from = static_cast<float>(rightToLeft ? rect.right : rect.left);
        const auto to = static_cast<float>(rightToLeft ? rect.left : rect.right);
        std::vector<TextStop> stops;
        stops.reserve(text.size() + 1);
        stops.push_back({from, offset});
        for (size_t index = 1; index < text.size(); ++index) {
            if (IS_LOW_SURROGATE(text[index]) ||
                (types[index] & (C3_NONSPACING | C3_VOWELMARK)) ||
                text[index] == L'\u200d' || text[index - 1] == L'\u200d') {
                continue;
            }
            const auto ratio = measured ? static_cast<float>(advances[index - 1]) / size.cx
                                        : static_cast<float>(index) / count;
            stops.push_back({from + (to - from) * std::clamp(ratio, 0.0f, 1.0f), offset + index});
        }
        stops.push_back({to, offset + text.size()});
        return stops;
    }

private:
    HDC dc_ = CreateCompatibleDC(nullptr);
    HFONT font_ = nullptr;
    HGDIOBJ oldFont_ = nullptr;
};

void AddFragment(std::vector<TextFragment>& fragments, std::wstring text,
                 const std::vector<RecognizedWord>& words, const WordMetrics& metrics) {
    if (words.empty()) {
        return;
    }
    std::vector<size_t> offsets;
    offsets.reserve(words.size());
    size_t next = 0;
    for (const auto& word : words) {
        const auto offset = text.find(word.text, next);
        if (offset == std::wstring::npos) {
            break;
        }
        offsets.push_back(offset);
        next = offset + word.text.size();
    }
    if (offsets.size() != words.size()) {
        text.clear();
        offsets.clear();
        for (const auto& word : words) {
            if (!text.empty()) {
                text += L' ';
            }
            offsets.push_back(text.size());
            text += word.text;
        }
    }
    TextLine line;
    line.words.reserve(words.size());
    for (size_t index = 0; index < words.size(); ++index) {
        const auto& word = words[index];
        UnionRect(&line.bounds, &line.bounds, &word.bounds);
        line.words.push_back({metrics.Stops(word.text, word.bounds, offsets[index]), word.bounds});
    }
    fragments.push_back({std::move(line), std::move(text)});
}

TextLayout Recognize(const MultilingualOcr& engine, const Image& image, std::stop_token cancel = {}) {
    // Preserve the original glyph shapes; resampling reduces screen-text accuracy.
    const WordMetrics metrics;
    std::vector<TextFragment> fragments;
    for (auto& line : engine.Recognize(image, cancel)) {
        for (auto& word : line) {
            OffsetRect(&word.bounds, image.origin.x, image.origin.y);
        }
        AddFragment(fragments, {}, line, metrics);
    }
    return TextLayout::FromFragments(std::move(fragments));
}
}

OcrWorker::OcrWorker(HWND owner, UINT resultMessage)
    : thread_([this, owner, resultMessage](std::stop_token stop) { Run(stop, owner, resultMessage); }) {}

OcrWorker::~OcrWorker() {
    thread_.request_stop();
    const std::lock_guard lock(mutex_);
    activeScan_.request_stop();
}

void OcrWorker::Submit(size_t captureId, Image image) {
    {
        const std::lock_guard lock(mutex_);
        activeScan_.request_stop();
        pending_ = Job{captureId, std::move(image)};
    }
    ready_.notify_one();
}

std::optional<OcrResult> OcrWorker::TakeResult() {
    const std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}

void OcrWorker::Cancel() {
    const std::lock_guard lock(mutex_);
    activeScan_.request_stop();
    pending_.reset();
    result_.reset();
}

void OcrWorker::Run(std::stop_token stop, HWND owner, UINT resultMessage) {
    std::optional<MultilingualOcr> multilingual;
    try {
        multilingual.emplace();
    } catch (...) {
        // Report startup failures through the regular result path.
    }
    while (!stop.stop_requested()) {
        std::optional<Job> job;
        std::stop_token cancel;
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait(lock, stop, [this] { return pending_.has_value(); })) {
                break;
            }
            job = std::exchange(pending_, std::nullopt);
            activeScan_ = std::stop_source{};
            cancel = activeScan_.get_token();
        }

        OcrResult result{job->captureId, {}};
        try {
            if (multilingual && *multilingual) {
                result.layout = Recognize(*multilingual, job->image, cancel);
            } else {
                result.error = L"OCR is unavailable.";
            }
        } catch (...) {
            result.error = L"Text recognition failed.";
        }

        {
            const std::lock_guard lock(mutex_);
            if (pending_ || cancel.stop_requested() || stop.stop_requested()) {
                continue;
            }
            result_ = std::move(result);
        }
        PostMessageW(owner, resultMessage, 0, 0);
    }
}

bool CopyText(HWND owner, std::wstring_view text) {
    const auto size = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) {
        return false;
    }
    auto* data = static_cast<wchar_t*>(GlobalLock(memory));
    if (!data) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(data, text.data(), text.size() * sizeof(wchar_t));
    data[text.size()] = L'\0';
    GlobalUnlock(memory);
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(owner)) {
            const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
            CloseClipboard();
            if (!copied) {
                GlobalFree(memory);
            }
            return copied;
        }
        Sleep(10);
    }
    GlobalFree(memory);
    return false;
}
