#pragma once

// The extractor's interaction surface, so one runtime drives both stub tiers.
//
// The full stub supplies a dialog-backed implementation; the mini stub supplies
// a console-backed one that cannot prompt at all. Keeping this an interface is
// what stops the mini stub from dragging in the Win32 dialog stack, which is
// most of the size difference between the two.

#include "axiom/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace axiom::sfx {

struct SfxSummary {
    std::wstring archive_name;
    std::wstring window_title;
    std::wstring banner_text;
    std::wstring description;
    std::size_t file_count = 0;
    std::size_t directory_count = 0;
    std::uint64_t unpacked_size = 0;
    bool encrypted = false;
    bool signature_present = false;
    bool signature_valid = false;
    std::wstring comment;
};

struct SfxChoices {
    std::filesystem::path destination;
    ExtractOptions::Overwrite overwrite = ExtractOptions::Overwrite::overwrite;
    std::size_t thread_count = 0;
    bool restore_mtime = true;
    bool open_destination = true;
    bool allow_path_change = true;
};

class SfxUi {
public:
    virtual ~SfxUi() = default;

    // False when this tier cannot ask the user anything, in which case every
    // required answer has to arrive from the command line or the config.
    virtual bool supports_prompts() const = 0;

    // Applies the package's `theme` preference, overriding what the system
    // reports. A console tier has nothing to apply.
    virtual void set_theme(bool /*dark*/) {}

    virtual bool ask_password(std::wstring& password) = 0;
    virtual bool ask_license(const std::wstring& title,
                             const std::wstring& text) = 0;
    virtual bool ask_options(const SfxSummary& summary, SfxChoices& choices) = 0;

    // Runs `work` on a worker thread, pumping whatever UI this tier has until
    // it finishes. A console tier simply runs it.
    virtual void run_with_progress(
        const std::shared_ptr<OperationControl>& operation,
        const std::function<void()>& work) = 0;

    virtual void message(const std::wstring& title, const std::wstring& text,
                         bool error) = 0;

    // Opens a finished destination in the shell, where that makes sense.
    virtual void reveal(const std::filesystem::path& destination) = 0;

    // Text destined for a console or a redirected pipe. Always available; a
    // dialog tier falls back to a message box when nothing is attached.
    virtual void write(const std::wstring& text) = 0;
    virtual void flush(const std::wstring& title, bool error) = 0;
};

}  // namespace axiom::sfx
