#include "bigred_archive.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace bigred {

namespace {

Blob decompress_rle(const uint8_t* data, size_t size, uint32_t uncompressed_size) {
    Blob out;
    out.reserve(uncompressed_size);
    size_t i = 0;
    while (i < size) {
        uint8_t flag = data[i++];
        if (flag > 127) {
            int count = flag - 125;
            if (i >= size) throw std::runtime_error("RLE: truncated repeat run");
            out.insert(out.end(), static_cast<size_t>(count), data[i]);
            i += 1;
        } else {
            int count = flag + 1;
            if (i + static_cast<size_t>(count) > size) throw std::runtime_error("RLE: truncated literal run");
            out.insert(out.end(), data + i, data + i + count);
            i += static_cast<size_t>(count);
        }
    }
    if (out.size() != uncompressed_size) {
        throw std::runtime_error("RLE output size mismatch");
    }
    return out;
}

Blob decompress_lzss(const uint8_t* data, size_t size, uint32_t uncompressed_size, int compression_level) {
    const int offset_bits = 6 - compression_level;
    const int run_length_bits = 8 - offset_bits;
    const int max_run_length = (1 << run_length_bits) + 2;
    const int max_offset = (1 << (offset_bits + 8)) - 1;

    Blob out(uncompressed_size);
    size_t out_len = 0;
    // +1 vs. max_offset: the reference C implementation's wraparound check lets
    // circular_offset reach max_offset itself before wrapping - see tools/bigred_archive.py's
    // matching comment.
    Blob circular(static_cast<size_t>(max_offset) + 1);
    int circular_offset = 0;

    size_t i = 0;
    const size_t n = size;
    while (i < n) {
        uint8_t flag = data[i++];
        for (int bit = 0; bit < 8; ++bit) {
            if ((flag >> bit) & 1) {
                if (i >= n) throw std::runtime_error("LZSS: truncated literal");
                uint8_t byte = data[i++];
                out[out_len++] = byte;
                circular[circular_offset] = byte;
                circular_offset = (circular_offset < max_offset) ? circular_offset + 1 : 0;
            } else {
                if (i + 1 >= n) throw std::runtime_error("LZSS: truncated back-reference");
                uint8_t b0 = data[i];
                uint8_t b1 = data[i + 1];
                i += 2;

                int offset = -1;
                int count = 0;
                for (int k = 0; k < 8; ++k) {
                    if ((b0 >> k) & 1) offset += (1 << count);
                    count += 1;
                }
                int run_length = 2;
                // `count` intentionally NOT reset here - it carries over from the b0 loop
                // above (ending at 8), matching the Python port exactly; it's only reset to 0
                // once k reaches offset_bits, below. A stray reset here was the original bug.
                for (int k = 0; k < 8; ++k) {
                    if (k == offset_bits) count = 0;
                    if ((b1 >> k) & 1) {
                        if (k >= offset_bits) run_length += (1 << count);
                        else offset += (1 << count);
                    }
                    count += 1;
                }

                if (offset < 0) throw std::runtime_error("LZSS: invalid offset");
                if (run_length > max_run_length) throw std::runtime_error("LZSS: invalid run length");

                for (int r = 0; r < run_length; ++r) {
                    if (offset > max_offset || static_cast<size_t>(offset) >= out_len) {
                        throw std::runtime_error("LZSS: invalid offset during copy");
                    }
                    uint8_t byte = circular[static_cast<size_t>(offset)];
                    circular[circular_offset] = byte;
                    circular_offset = (circular_offset < max_offset) ? circular_offset + 1 : 0;
                    out[out_len++] = byte;
                    offset = (offset < max_offset) ? offset + 1 : 0;
                }
            }
            if (i >= n) break;
        }
    }

    if (out_len != uncompressed_size) {
        throw std::runtime_error("LZSS output size mismatch");
    }
    return out;
}

} // namespace

std::unordered_map<std::string, Blob> unpack_bytes(const uint8_t* data, size_t size) {
    std::unordered_map<std::string, Blob> files;
    size_t pos = 0;
    while (pos < size) {
        // Null-terminated filename.
        size_t end = pos;
        while (end < size && data[end] != 0) ++end;
        if (end >= size) throw std::runtime_error("archive: unterminated filename");
        if (end == pos) break; // empty filename = end-of-file marker
        std::string filename(reinterpret_cast<const char*>(data + pos), end - pos);
        pos = end + 1;

        if (pos + 9 > size) throw std::runtime_error("archive: truncated entry header for " + filename);
        uint32_t compressed_size, uncompressed_size;
        std::memcpy(&compressed_size, data + pos, 4);
        std::memcpy(&uncompressed_size, data + pos + 4, 4);
        pos += 8;
        uint8_t compression_type = data[pos];
        pos += 1;

        if (pos + compressed_size > size) throw std::runtime_error("archive: truncated chunk for " + filename);
        const uint8_t* chunk = data + pos;
        pos += compressed_size;

        if (compression_type == 0) {
            files[filename] = Blob(chunk, chunk + compressed_size);
        } else if (compression_type == 1) {
            files[filename] = decompress_rle(chunk, compressed_size, uncompressed_size);
        } else if (compression_type > 1) {
            files[filename] = decompress_lzss(chunk, compressed_size, uncompressed_size, compression_type);
        } else {
            throw std::runtime_error("archive: unsupported compression type for " + filename);
        }
    }
    return files;
}

namespace {

// std::ifstream's narrow-char path constructor interprets bytes via the current Windows ANSI
// codepage, not UTF-8 - source literals/paths containing non-ASCII characters (this repo lives
// under "H:\Décompilation\...") get mojibake'd and silently fail to open. Explicitly convert
// UTF-8 -> UTF-16 and open via the wide-char API instead. Windows-only for now (matches this
// milestone's toolchain); revisit if/when this loader targets other platforms.
Blob read_file_bytes(const std::string& path_utf8) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) throw std::runtime_error("could not convert path to UTF-16: " + path_utf8);
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, wpath.data(), wlen);

    std::ifstream f(wpath.c_str(), std::ios::binary | std::ios::ate);
#else
    std::ifstream f(path_utf8, std::ios::binary | std::ios::ate);
#endif
    if (!f) throw std::runtime_error("could not open " + path_utf8);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    Blob data(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("could not read " + path_utf8);
    }
    return data;
}

} // namespace

Blob read_file_utf8(const std::string& path_utf8) {
    try {
        return read_file_bytes(path_utf8);
    } catch (const std::exception&) {
        return Blob();
    }
}

bool file_exists_utf8(const std::string& path_utf8) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, wpath.data(), wlen);
    std::ifstream f(wpath.c_str(), std::ios::binary);
#else
    std::ifstream f(path_utf8, std::ios::binary);
#endif
    return static_cast<bool>(f);
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& utf8) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), wlen);
    if (!w.empty() && w.back() == L'\0') w.pop_back();   // MultiByteToWideChar counts the NUL
    return w;
}
} // namespace
#endif

bool write_file_utf8(const std::string& path_utf8, const Blob& data) {
#ifdef _WIN32
    const std::wstring wpath = widen(path_utf8);
    if (wpath.empty()) return false;
    std::ofstream f(wpath.c_str(), std::ios::binary | std::ios::trunc);
#else
    std::ofstream f(path_utf8, std::ios::binary | std::ios::trunc);
#endif
    if (!f) return false;
    if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()),
                                static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

bool create_directories_utf8(const std::string& dir_utf8) {
#ifdef _WIN32
    const std::wstring wdir = widen(dir_utf8);
    if (wdir.empty()) return false;
    // Walk the path component by component - CreateDirectoryW does not create parents, and
    // there is no dependency on Shell32 this way. Both separators: callers may build a path
    // with '/' (this project mixes both, e.g. game_dir arguments).
    std::wstring partial;
    for (size_t i = 0; i < wdir.size(); ++i) {
        const wchar_t c = wdir[i];
        partial += c;
        if ((c == L'\\' || c == L'/') || i + 1 == wdir.size()) {
            // Skip a bare drive letter ("C:") and UNC leading slashes - CreateDirectoryW
            // rejects both as "already exists" in practice, but skipping avoids relying on it.
            if (partial.size() <= 3 && partial.find(L':') != std::wstring::npos) continue;
            if (!CreateDirectoryW(partial.c_str(), nullptr)) {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) return false;
            }
        }
    }
    return GetFileAttributesW(wdir.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    (void)dir_utf8;
    return false;
#endif
}

std::unordered_map<std::string, Blob> unpack_file(const std::string& path) {
    Blob data = read_file_bytes(path);
    return unpack_bytes(data.data(), data.size());
}

} // namespace bigred
