#include "metadata_internal.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {
constexpr std::uint32_t kReplacement = 0xFFFD;

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

std::u16string to_utf16(std::string_view input) {
    std::u16string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size();) {
        const auto lead = static_cast<unsigned char>(input[index]);
        std::uint32_t code_point = 0;
        std::size_t continuation_count = 0;
        if (lead < 0x80) {
            code_point = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1F;
            continuation_count = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0F;
            continuation_count = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07;
            continuation_count = 3;
        } else {
            output.push_back(static_cast<char16_t>(kReplacement));
            ++index;
            continue;
        }

        bool valid = true;
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            if (index + offset >= input.size()) {
                valid = false;
                break;
            }
            const auto continuation = static_cast<unsigned char>(input[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        index += continuation_count + 1;
        if (!valid || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            output.push_back(static_cast<char16_t>(kReplacement));
            continue;
        }
        if (code_point <= 0xFFFF) {
            output.push_back(static_cast<char16_t>(code_point));
        } else {
            code_point -= 0x10000;
            output.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
            output.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
        }
    }
    return output;
}

} /// namespace

namespace INB::detail {

std::string utf16_to_utf8(std::u16string_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint32_t code_point = input[index];
        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            if (index + 1 < input.size()) {
                const std::uint32_t low = input[index + 1];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                    ++index;
                } else {
                    code_point = kReplacement;
                }
            } else {
                code_point = kReplacement;
            }
        } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            code_point = kReplacement;
        }
        append_utf8(output, code_point);
    }
    return output;
}

} /// namespace INB::detail

namespace INB {

Result<std::u16string> Runtime::string_to_utf16(Il2CppString* value) const {
    if (value == nullptr) {
        return Result<std::u16string>::failure(make_error(ErrorCode::invalid_argument,
                                                         "managed string must not be null"));
    }
    auto attached = detail::acquire_thread(context_);
    if (!attached) return Result<std::u16string>::failure(attached.error());

    const auto api = detail::api_for(context_);
    if (api.string_length == nullptr || api.string_chars == nullptr) {
        return Result<std::u16string>::failure(make_error(ErrorCode::missing_api,
                                                         "il2cpp string reading exports are unavailable"));
    }
    const auto length = api.string_length(value);
    if (length < 0) {
        return Result<std::u16string>::failure(make_error(ErrorCode::invocation_failed,
                                                         "managed string returned a negative length"));
    }
    if (length == 0) return Result<std::u16string>::success(std::u16string{});

    const auto* chars = api.string_chars(value);
    if (chars == nullptr) {
        return Result<std::u16string>::failure(make_error(ErrorCode::invocation_failed,
                                                         "managed string returned no character data"));
    }
    return Result<std::u16string>::success(
            std::u16string(reinterpret_cast<const char16_t*>(chars), static_cast<std::size_t>(length)));
}

Result<std::string> Runtime::string_to_utf8(Il2CppString* value) const {
    auto utf16 = string_to_utf16(value);
    if (!utf16) return Result<std::string>::failure(utf16.error());
    return Result<std::string>::success(detail::utf16_to_utf8(utf16.value()));
}

Result<Il2CppString*> Runtime::string_from_utf16(std::u16string_view value) const {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Result<Il2CppString*>::failure(make_error(ErrorCode::invalid_argument,
                                                        "managed string is too long"));
    }
    auto attached = detail::acquire_thread(context_);
    if (!attached) return Result<Il2CppString*>::failure(attached.error());

    const auto api = detail::api_for(context_);
    if (api.string_new_utf16 == nullptr) {
        return Result<Il2CppString*>::failure(make_error(ErrorCode::missing_api,
                                                        "il2cpp_string_new_utf16 is unavailable"));
    }
    const std::uint16_t empty[] = {0};
    const auto* chars = value.empty() ? empty : reinterpret_cast<const std::uint16_t*>(value.data());
    auto* result = api.string_new_utf16(chars, static_cast<std::uint32_t>(value.size()));
    if (result == nullptr) {
        return Result<Il2CppString*>::failure(make_error(ErrorCode::invocation_failed,
                                                        "could not allocate the managed string"));
    }
    return Result<Il2CppString*>::success(result);
}

Result<Il2CppString*> Runtime::string_from_utf8(std::string_view value) const {
    return string_from_utf16(to_utf16(value));
}

} /// namespace INB
