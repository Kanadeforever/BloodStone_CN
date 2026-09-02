#include "font_atlas.h"
#include "log.h"

namespace bscn {
namespace {
#pragma pack(push, 1)
struct AtlasHeaderDisk {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint32_t line_height;
    uint32_t ascent;
    uint32_t glyph_count;
    uint32_t glyph_record_size;
    uint32_t pixels_offset;
    uint32_t pixels_size;
    uint32_t pixel_format;
    uint32_t token_width;
    uint8_t token_prefix0;
    uint8_t token_prefix1;
    uint8_t token_alphabet;
    uint8_t reserved_byte;
    uint32_t reserved;
};
struct GlyphRecordDisk {
    uint32_t codepoint;
    uint32_t carrier_token;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    int16_t bearing_x;
    int16_t bearing_y;
    int16_t advance_x;
    uint16_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
};
#pragma pack(pop)
static_assert(sizeof(AtlasHeaderDisk) == 64, "BFA header layout changed");
static_assert(sizeof(GlyphRecordDisk) == 32, "BFA glyph layout changed");
constexpr char kAtlasMagic[8] = {'B','S','C','N','A','T','L','1'};
constexpr uint32_t kAtlasVersion = 1;
constexpr uint32_t kPixelFormatA8 = 1;
constexpr uint8_t kBase36Alphabet = 1;

bool IsUnicodeScalar(uint32_t value) {
    return value <= 0x10FFFFu && !(value >= 0xD800u && value <= 0xDFFFu);
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* output) {
    output->clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) != FALSE && size.QuadPart >= 0 &&
              size.QuadPart <= 64ll * 1024ll * 1024ll;
    if (ok) {
        output->resize(static_cast<size_t>(size.QuadPart));
        size_t pos = 0;
        while (pos < output->size()) {
            DWORD want = static_cast<DWORD>(std::min<size_t>(output->size() - pos, 1u << 20));
            DWORD got = 0;
            if (!ReadFile(f, output->data() + pos, want, &got, nullptr) || got != want) {
                ok = false;
                break;
            }
            pos += got;
        }
    }
    CloseHandle(f);
    if (!ok) output->clear();
    return ok;
}
}

ExternalFontAtlas::~ExternalFontAtlas() { Shutdown(); }

bool ExternalFontAtlas::LoadCpuResources(const std::wstring& module_directory,
                                         const Config& config) {
    Shutdown();
    atlas_path_ = JoinPath(module_directory, config.atlas_file);
    BSCN_LOG_INFO("BFA_CPU_BEGIN path=%s", WideToUtf8(atlas_path_).c_str());
    if (!LoadFile(atlas_path_)) {
        BSCN_LOG_ERROR("BFA_CPU_FAILED path=%s", WideToUtf8(atlas_path_).c_str());
        return false;
    }
    BSCN_LOG_INFO("BFA_CPU_OK glyphs=%u size=%dx%d line=%d ascent=%d pixels=%u",
                  static_cast<unsigned>(glyphs_.size()), atlas_width_, atlas_height_,
                  line_height_, ascent_, static_cast<unsigned>(pixels_.size()));
    return true;
}

bool ExternalFontAtlas::LoadFile(const std::wstring& path) {
    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, &file) || file.size() < sizeof(AtlasHeaderDisk)) return false;
    AtlasHeaderDisk h{};
    memcpy(&h, file.data(), sizeof(h));
    const uint64_t records_size = static_cast<uint64_t>(h.glyph_count) * h.glyph_record_size;
    const uint64_t min_pixels = static_cast<uint64_t>(h.header_size) + records_size;
    const uint64_t expected_pixels = static_cast<uint64_t>(h.width) * h.height;
    const uint64_t pixels_end = static_cast<uint64_t>(h.pixels_offset) + h.pixels_size;
    const bool valid =
        memcmp(h.magic, kAtlasMagic, sizeof(kAtlasMagic)) == 0 &&
        h.version == kAtlasVersion && h.header_size == sizeof(AtlasHeaderDisk) &&
        h.glyph_record_size == sizeof(GlyphRecordDisk) &&
        h.pixel_format == kPixelFormatA8 && h.token_width == kCarrierDigits &&
        h.token_prefix0 == 'Q' && h.token_prefix1 == 'Z' &&
        h.token_alphabet == kBase36Alphabet &&
        h.width > 0 && h.width <= 8192 && h.height > 0 && h.height <= 8192 &&
        h.glyph_count > 0 && h.glyph_count <= 200000 &&
        min_pixels <= h.pixels_offset && expected_pixels == h.pixels_size &&
        pixels_end <= file.size();
    if (!valid) return false;

    std::unordered_map<char32_t, GlyphInfo> glyphs;
    glyphs.reserve(h.glyph_count);
    for (uint32_t i = 0; i < h.glyph_count; ++i) {
        GlyphRecordDisk r{};
        const size_t off = h.header_size + static_cast<size_t>(i) * h.glyph_record_size;
        memcpy(&r, file.data() + off, sizeof(r));
        if (!IsUnicodeScalar(r.codepoint) ||
            static_cast<uint32_t>(r.x) + r.width > h.width ||
            static_cast<uint32_t>(r.y) + r.height > h.height ||
            glyphs.find(static_cast<char32_t>(r.codepoint)) != glyphs.end()) {
            return false;
        }
        GlyphInfo g{};
        g.codepoint = static_cast<char32_t>(r.codepoint);
        g.carrier_token = r.carrier_token;
        g.atlas_x = r.x; g.atlas_y = r.y;
        g.width = r.width; g.height = r.height;
        g.bearing_x = r.bearing_x; g.bearing_y = r.bearing_y;
        g.advance_x = r.advance_x;
        g.u0 = static_cast<float>(r.x) / static_cast<float>(h.width);
        g.v0 = static_cast<float>(r.y) / static_cast<float>(h.height);
        g.u1 = static_cast<float>(r.x + r.width) / static_cast<float>(h.width);
        g.v1 = static_cast<float>(r.y + r.height) / static_cast<float>(h.height);
        glyphs.emplace(g.codepoint, g);
    }
    atlas_width_ = static_cast<int>(h.width);
    atlas_height_ = static_cast<int>(h.height);
    line_height_ = static_cast<int>(h.line_height);
    ascent_ = static_cast<int>(h.ascent);
    pixels_.assign(file.begin() + h.pixels_offset, file.begin() + pixels_end);
    glyphs_.swap(glyphs);
    loaded_ = true;
    return true;
}

bool ExternalFontAtlas::CreateDeviceResources(IDirect3DDevice9* device) {
    if (!loaded_ || !device) return false;
    if (texture_) { texture_->Release(); texture_ = nullptr; }
    device_ = device;
    return CreateTexture();
}

bool ExternalFontAtlas::CreateTexture() {
    if (!device_ || pixels_.empty()) return false;
    HRESULT hr = device_->CreateTexture(static_cast<UINT>(atlas_width_),
                                        static_cast<UINT>(atlas_height_), 1, 0,
                                        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                        &texture_, nullptr);
    if (FAILED(hr) || !texture_) return false;
    D3DLOCKED_RECT locked{};
    hr = texture_->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) { texture_->Release(); texture_ = nullptr; return false; }
    for (int y = 0; y < atlas_height_; ++y) {
        auto* dst = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
        const uint8_t* src = pixels_.data() + static_cast<size_t>(y) * atlas_width_;
        for (int x = 0; x < atlas_width_; ++x) {
            const uint32_t a = src[x];
            dst[x] = (a << 24u) | (a << 16u) | (a << 8u) | a;
        }
    }
    texture_->UnlockRect(0);
    BSCN_LOG_INFO("BFA_GPU_OK texture=%p", texture_);
    return true;
}

void ExternalFontAtlas::OnBeforeReset() {
    // 历史 M2F1 使用 MANAGED 纹理。为保持原路径，Reset 前仍主动释放，
    // Reset 后从已保留的 CPU 像素重新创建。
    if (texture_) { texture_->Release(); texture_ = nullptr; }
}

bool ExternalFontAtlas::OnAfterReset(IDirect3DDevice9* device) {
    return loaded_ && CreateDeviceResources(device);
}

void ExternalFontAtlas::Shutdown() {
    if (texture_) { texture_->Release(); texture_ = nullptr; }
    device_ = nullptr;
    loaded_ = false;
    atlas_width_ = atlas_height_ = 0;
    line_height_ = ascent_ = 1;
    pixels_.clear();
    glyphs_.clear();
}

const GlyphInfo* ExternalFontAtlas::FindGlyphExact(char32_t codepoint) const {
    const auto it = glyphs_.find(codepoint);
    return it == glyphs_.end() ? nullptr : &it->second;
}

}  // namespace bscn
