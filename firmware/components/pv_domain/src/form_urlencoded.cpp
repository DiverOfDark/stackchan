#include "domain/form_urlencoded.hpp"

#include <cctype>

namespace domain {

namespace {

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

} // namespace

std::string urlDecode(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            int hi = hexValue(in[i + 1]);
            int lo = hexValue(in[i + 2]);
            if (hi < 0 || lo < 0) {
                // Malformed escape — emit literal % and keep going.
                out.push_back(c);
            } else {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<std::string> formGet(std::string_view body, std::string_view key)
{
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t amp = body.find('&', pos);
        const std::size_t end = (amp == std::string_view::npos) ? body.size() : amp;
        const std::size_t eq  = body.find('=', pos);
        if (eq != std::string_view::npos && eq < end) {
            std::string_view k = body.substr(pos, eq - pos);
            if (k == key) {
                return urlDecode(body.substr(eq + 1, end - eq - 1));
            }
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return std::nullopt;
}

} // namespace domain
