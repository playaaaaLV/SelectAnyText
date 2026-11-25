#include "transfer.h"
#include "multilingual_ocr.h"
#include "ocr_color.h"
#include "text_angle.h"

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

using OcrRows = std::vector<std::vector<RecognizedWord>>;

double ReadingQuality(const OcrRows& rows) {
    double score = 0;
    for (const auto& row : rows) for (const auto& word : row) {
        if (word.confidence >= 50 && word.bounds.bottom - word.bounds.top <= word.bounds.right - word.bounds.left)
            score += (word.confidence - 45) * std::min<size_t>(OcrLetterCount(word.text), 16);
    }
    return score;
}

// Large display capitals leave sustained, repeated edges even when page
// segmentation returns only a small caption elsewhere in the image.
bool HasMissingTitle(const Image& image, const OcrRows& rows, std::stop_token cancel) {
    if (image.pixels.size() > 1000000 || image.height < 32 || image.width < 24) return false;
    std::vector<bool> covered(image.height);
    for (const auto& row : rows) for (const auto& word : row) {
        if (word.confidence < 60) continue;
        for (int y = std::max(0L, word.bounds.top); y < std::min<LONG>(image.height, word.bounds.bottom); ++y)
            covered[y] = true;
    }
    int run = 0;
    for (int y = 0; y < image.height; ++y) {
        if (cancel.stop_requested()) return false;
        int transitions = 0;
        if (!covered[y]) {
            const auto* p = image.pixels.data() + size_t(y) * image.width;
            for (int x = 2; x < image.width; x += 2)
                transitions += std::abs(text_angle::Luma(p[x]) - text_angle::Luma(p[x - 2])) > 45;
        }
        run = transitions >= 4 ? run + 1 : 0;
        if (run >= 30) return true;
    }
    return false;
}

bool CompleteTitles(const MultilingualOcr& engine, const Image& image, OcrRows& words, std::stop_token cancel) {
    bool recoveredTitle = false;
    if (HasMissingTitle(image, words, cancel)) {
        const auto titles = engine.Recognize(image, cancel, OcrPass::Display);
        for (const auto& row : titles) for (const auto& word : row) {
            if (word.confidence < 70) continue;
            const auto overlaps = [&](const auto& existing) {
                RECT overlap{};
                return std::any_of(existing.begin(), existing.end(), [&](const auto& other) {
                    if (!IntersectRect(&overlap, &word.bounds, &other.bounds)) return false;
                    const auto area = [](RECT r) { return double(r.right-r.left) * (r.bottom-r.top); };
                    return area(overlap) >= .4 * std::min(area(word.bounds), area(other.bounds));
                });
            };
            if (std::none_of(words.begin(), words.end(), overlaps)) {
                words.push_back({word});
                recoveredTitle |= word.bounds.bottom - word.bounds.top >= 30 && word.confidence >= 85;
            }
        }
    }
    return recoveredTitle;
}

TextLayout Recognize(const MultilingualOcr& engine, const Image& image, std::stop_token cancel = {}) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) return {};
    auto words = engine.Recognize(image, cancel);
    ReadingFrame frame{double(image.origin.x), double(image.origin.y)};
    const double angle = text_angle::Estimate(image, cancel);
    const bool inclined = std::abs(angle) >= .5 && std::abs(angle) <= 89.5;
    const bool recoveredTitle = !inclined && CompleteTitles(engine, image, words, cancel);
    double best = ReadingQuality(words);
    Image winningRaster;
    const auto consider = [&](double degrees, OcrPass pass, double minimum) {
        if (cancel.stop_requested()) return false;
        auto aligned = text_angle::Align(image, degrees, cancel);
        if (aligned.image.pixels.empty()) return false;
        auto candidate = engine.Recognize(aligned.image, cancel, pass);
        const auto score = ReadingQuality(candidate);
        if (score < 60 || score < minimum) return false;
        best = score;
        words = std::move(candidate);
        frame = aligned.frame;
        if (pass == OcrPass::AlignedLine) winningRaster = std::move(aligned.image);
        return true;
    };
    bool aligned = false;
    if (inclined) {
        // An already readable, lightly tilted line still needs its true baseline.
        for (double offset : {0., 180., 90., -90.}) {
            if (consider(angle + offset, OcrPass::AlignedLine, best * (aligned ? 1.04 : .94)))
                aligned = true;
        }
        if (aligned) CompleteTitles(engine, winningRaster, words, cancel);
    }
    const bool tallWord = std::any_of(words.begin(), words.end(), [](const auto &row) {
        return std::any_of(row.begin(), row.end(), [](const auto &word) {
            return word.text.size() >= 3 &&
                   word.bounds.bottom - word.bounds.top > 2 * (word.bounds.right - word.bounds.left);
        });
    });
    if (!aligned && ((image.height > image.width && !recoveredTitle) || tallWord ||
                     (words.empty() && image.height * 2 >= image.width))) {
        consider(90, OcrPass::Display, best * 1.12);
        consider(-90, OcrPass::Display, best * 1.12);
    }
    // Inverted type can yield a few confident but incorrect short words.
    // Compare both directions for short horizontal selections as well as empty ones.
    if (!inclined && frame.sine == 0 && (best < 65 || (std::abs(angle) < 1 &&
        words.size() <= 3 && image.pixels.size() <= 1000000)) && !cancel.stop_requested())
        consider(180, OcrPass::AlignedLine, best * 1.12);
    if (cancel.stop_requested()) return {};
    const WordMetrics metrics;
    std::vector<TextFragment> fragments;
    for (const auto& line : words) AddFragment(fragments, {}, line, metrics);
    return TextLayout::FromFragments(std::move(fragments), frame);
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
