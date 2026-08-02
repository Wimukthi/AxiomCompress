#include "sfx/module_file.hpp"

#include "core/path_text.hpp"

#include <stdexcept>
#include <system_error>
#include <vector>

namespace axiom::sfx {
namespace {

std::filesystem::path application_path(HINSTANCE module) {
    if (module == nullptr) module = GetModuleHandleW(nullptr);

    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "cannot locate the Axiom application");
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(buffer.data(), buffer.data() + length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace

std::filesystem::path module_file_path(HINSTANCE application_module,
                                       SfxStubTier tier) {
    const wchar_t* name =
        tier == SfxStubTier::mini ? L"AxiomSfxMini.bin" : L"AxiomSfx.bin";
    return application_path(application_module).parent_path() / name;
}

void create_from_module_file(
    HINSTANCE application_module,
    const std::filesystem::path& archive_path,
    const std::filesystem::path& output_executable,
    const std::shared_ptr<OperationControl>& operation,
    std::size_t io_buffer_size,
    std::span<const std::uint8_t> config,
    SfxStubTier tier) {
    const std::filesystem::path module_path =
        module_file_path(application_module, tier);
    std::error_code error;
    if (!std::filesystem::is_regular_file(module_path, error)) {
        throw std::runtime_error(
            core::path_to_utf8(module_path.filename()) +
            " is missing from the Axiom application folder");
    }
    create_sfx_archive(archive_path, module_path, output_executable, operation,
                       io_buffer_size, config);
}

}  // namespace axiom::sfx
