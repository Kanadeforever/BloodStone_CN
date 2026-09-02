#pragma once
#include "common.h"
#include "config.h"

namespace bscn {

constexpr uint32_t kNoCarrierToken = 0xFFFFFFFFu;
constexpr uint32_t kCarrierRadix = 36;
constexpr uint32_t kCarrierDigits = 4;
constexpr uint32_t kCarrierCapacity =
    kCarrierRadix * kCarrierRadix * kCarrierRadix * kCarrierRadix;

struct GlyphInfo {
    char32_t codepoint = 0;
    uint32_t carrier_token = kNoCarrierToken;
    int atlas_x = 0;
    int atlas_y = 0;
    int width = 0;
    int height = 0;
    int bearing_x = 0;
    int bearing_y = 0;
    int advance_x = 0;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
};

class ExternalFontAtlas {
public:
    ~ExternalFontAtlas();
    bool LoadCpuResources(const std::wstring& module_directory,
                          const Config& config);
    bool CreateDeviceResources(IDirect3DDevice9* device);
    void Shutdown();
    void OnBeforeReset();
    bool OnAfterReset(IDirect3DDevice9* device);
    const GlyphInfo* FindGlyphExact(char32_t codepoint) const;
    IDirect3DTexture9* texture() const { return texture_; }
    bool loaded() const { return loaded_; }
    int width() const { return atlas_width_; }
    int height() const { return atlas_height_; }
    size_t glyph_count() const { return glyphs_.size(); }
private:
    bool LoadFile(const std::wstring& path);
    bool CreateTexture();
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DTexture9* texture_ = nullptr;
    std::wstring atlas_path_;
    bool loaded_ = false;
    int atlas_width_ = 0;
    int atlas_height_ = 0;
    int line_height_ = 1;
    int ascent_ = 1;
    std::vector<uint8_t> pixels_;
    std::unordered_map<char32_t, GlyphInfo> glyphs_;
};

}  // namespace bscn
