#pragma once

#include <string>

namespace Html {
// Returns the cached EPUB path, or empty on failure. The source is never modified.
// Uses ConvertedEpub's source-path/size/mtime fingerprint and transactional cache.
// Normalizes UTF-8 (or declared Latin-1/Windows-1252) HTML into one XHTML spine
// item. Headings supply the TOC; semantic formatting, tables and fragment links
// use the native EPUB renderer. Local PNG/JPEG resources are packaged, never
// fetched. Missing/unsupported/remote images retain their alt text.
// Memory is independent of source size: 1 KiB input, 2 KiB tag and 1 KiB path
// buffers, 48 open tags, 255-byte metadata; at most 256 TOC entries/128 images.
// Oversized tags lose attributes, excess nesting is flattened, and text remains.
// External CSS, scripts, frames and unsupported declared encodings are not read.
std::string prepareEpub(const std::string& sourcePath, const std::string& cacheRoot = "/.crosspoint");
}  // namespace Html
