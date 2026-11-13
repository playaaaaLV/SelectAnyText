#pragma once

#include "screen.h"

#include <condition_variable>
#include <mutex>
#include <thread>

struct OcrResult {
    size_t captureId;
    TextLayout layout;
    const wchar_t* error = nullptr;
};

class OcrWorker {
public:
    OcrWorker(HWND owner, UINT resultMessage);
    ~OcrWorker();

    void Submit(size_t captureId, Image image);
    void Cancel();
    std::optional<OcrResult> TakeResult();

private:
    struct Job {
        size_t captureId;
        Image image;
    };

    void Run(std::stop_token stop, HWND owner, UINT resultMessage);

    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::optional<Job> pending_;
    std::optional<OcrResult> result_;
    std::stop_source activeScan_;
    std::jthread thread_;
};

bool CopyText(HWND owner, std::wstring_view text);
