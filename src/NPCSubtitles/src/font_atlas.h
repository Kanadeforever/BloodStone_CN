#pragma once
#include "common.h"
namespace npcsub {
struct GlyphInfo {
    char32_t codepoint = 0;
    int width = 0, height = 0, bearing_x = 0, bearing_y = 0, advance_x = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};
class FontAtlas {
public:
    ~FontAtlas();
    bool Load(const std::wstring& path);
    bool EnsureTexture(IDirect3DDevice9* device);
    void InvalidateGpuTexture();
    void Shutdown();
    const GlyphInfo* Find(char32_t codepoint) const;
    IDirect3DTexture9* texture() const { return texture_; }
    bool loaded() const { return loaded_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int line_height() const { return line_height_; }
    int ascent() const { return ascent_; }
    size_t glyph_count() const { return glyphs_.size(); }
private:
    bool CreateTexture(IDirect3DDevice9* device);
    bool loaded_ = false;
    int width_ = 0, height_ = 0, line_height_ = 1, ascent_ = 1;
    std::vector<uint8_t> pixels_;
    std::unordered_map<char32_t,GlyphInfo> glyphs_;
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DTexture9* texture_ = nullptr;
};
}
