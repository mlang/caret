#include <mlang/syntax.hpp>
#include <mlang/text_buffer.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace mlang {

namespace {

namespace utf8 {

bool continuation(unsigned char byte) noexcept
{ return (byte & 0xc0) == 0x80; }

uint32_t previous_char_start(std::u8string_view text, uint32_t offset) noexcept
{
    offset = std::min<uint32_t>(offset, text.size());

    if (offset == 0) return 0;

    --offset;

    while (offset > 0 && continuation(static_cast<unsigned char>(text[offset]))) {
        --offset;
    }

    return offset;
}

uint32_t clamp_to_char_boundary(std::u8string_view text, uint32_t offset)
{
    offset = std::min<uint32_t>(offset, text.size());

    while (offset > 0
        && offset < text.size()
        && continuation(static_cast<unsigned char>(text[offset]))) {
        --offset;
    }

    return offset;
}

void append(std::u8string &out, char32_t cp)
{
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        cp = replacement_character;
    }

    if (cp <= 0x7f) {
        out.push_back(static_cast<char8_t>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char8_t>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char8_t>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char8_t>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3f)));
    }
}

struct Decoded
{
    wchar_t wc = replacement_character;
    uint32_t bytes = 1;
};

Decoded decode_one(std::u8string_view text)
{
    assert(!text.empty());

    const auto fail = [] {
        return Decoded{replacement_character, 1};
    };

    const auto b0 = static_cast<unsigned char>(text[0]);

    if (b0 < 0x80) {
        return Decoded{static_cast<wchar_t>(b0), 1};
    }

    char32_t cp = 0;
    char32_t min = 0;
    uint32_t n = 0;

    if (b0 >= 0xc2 && b0 <= 0xdf) {
        cp = b0 & 0x1f;
        min = 0x80;
        n = 2;
    } else if (b0 >= 0xe0 && b0 <= 0xef) {
        cp = b0 & 0x0f;
        min = 0x800;
        n = 3;
    } else if (b0 >= 0xf0 && b0 <= 0xf4) {
        cp = b0 & 0x07;
        min = 0x10000;
        n = 4;
    } else {
        return fail();
    }

    if (text.size() < n) {
        return fail();
    }

    for (uint32_t i = 1; i < n; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);

        if (!continuation(byte)) {
            return fail();
        }

        cp = (cp << 6) | (byte & 0x3f);
    }

    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        return fail();
    }

    return Decoded{static_cast<wchar_t>(cp), n};
}

std::wstring decode(std::u8string_view text)
{
    std::wstring out;

    while (!text.empty()) {
        const auto [wc, bytes] = decode_one(text);
        assert(bytes > 0);
        out.push_back(wc);
        text = text.substr(bytes);
    }

    return out;
}

} // namespace utf8

int character_width(wchar_t wc)
{
    const int width = wcwidth(wc);
    return width < 0 ? 1 : width;
}

[[nodiscard]] Syntax::Point advance_point(Syntax::Point p, std::u8string_view text) noexcept
{
    for (char8_t byte: text) {
        if (byte == u8'\n') {
            ++p.row;
            p.byte_column = 0;
        } else {
            ++p.byte_column;
        }
    }

    return p;
}

Syntax::Position syntax_position(uint32_t byte, Syntax::Point point) noexcept
{ return { .byte = {.index = byte}, .point = point }; }

[[nodiscard]] bool syntax_node_matches(
    const Syntax::Node &node,
    SyntaxNodeMode mode
) noexcept
{
    return mode == SyntaxNodeMode::Any || node.is_named();
}

[[nodiscard]] Syntax::PointRange syntax_node_range_unchecked(const Syntax::Node &node)
{
    const auto region = node.range();
    return { region.start.point, region.end.point };
}

std::optional<Syntax::PointRange> syntax_node_range(
    const Syntax::Node &node, SyntaxNodeMode mode
)
{
    if (!syntax_node_matches(node, mode)) return std::nullopt;

    auto range = syntax_node_range_unchecked(node);
    if (range.empty()) return std::nullopt;

    return range;
}

[[nodiscard]] std::optional<Syntax::Node> nearest_syntax_node(
    std::optional<Syntax::Node> node,
    SyntaxNodeMode mode
) {
    while (node) {
        if (syntax_node_range(*node, mode)) {
            return node;
        }

        node = node->parent();
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<Syntax::Node> syntax_node_for_range(
    const Syntax &syntax,
    Syntax::PointRange range,
    SyntaxNodeMode mode
) {
    auto node = mode == SyntaxNodeMode::Named
        ? syntax.named_descendant_for(range)
        : syntax.descendant_for(range);

    return nearest_syntax_node(std::move(node), mode);
}

[[nodiscard]] std::optional<Syntax::Node> next_syntax_sibling(
    const Syntax::Node &node,
    SyntaxNodeMode mode
) {
    auto sibling = mode == SyntaxNodeMode::Named
        ? node.next_named_sibling()
        : node.next_sibling();

    while (sibling) {
        if (syntax_node_range(*sibling, mode)) {
            return sibling;
        }

        sibling = mode == SyntaxNodeMode::Named
            ? sibling->next_named_sibling()
            : sibling->next_sibling();
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<Syntax::Node> previous_syntax_sibling(
    const Syntax::Node &node,
    SyntaxNodeMode mode
) {
    auto sibling = mode == SyntaxNodeMode::Named
        ? node.previous_named_sibling()
        : node.previous_sibling();

    while (sibling) {
        if (syntax_node_range(*sibling, mode)) {
            return sibling;
        }

        sibling = mode == SyntaxNodeMode::Named
            ? sibling->previous_named_sibling()
            : sibling->previous_sibling();
    }

    return std::nullopt;
}

[[nodiscard]] Syntax::PointRange wide_range(
    const TextBuffer &buffer, uint32_t row, std::size_t begin, std::size_t end
)
{
    return {
        {row, buffer.byte_column_for_wide_index(row, begin)},
        {row, buffer.byte_column_for_wide_index(row, end)},
    };
}

[[nodiscard]] std::optional<Syntax::PointRange> first_regex_match_in_line(
    const TextBuffer &buffer,
    const std::wstring &line,
    uint32_t row,
    const std::wregex &regex,
    std::size_t min_begin,
    std::size_t max_begin
) {
    if (line.empty() || min_begin > line.size()) return std::nullopt;

    for (std::size_t search_begin = min_begin; search_begin <= line.size();) {
        std::wsmatch match;
        const auto begin_it = line.cbegin()
            + static_cast<std::ptrdiff_t>(search_begin);
        const auto flags = search_begin > 0
            ? std::regex_constants::match_not_bol
            : std::regex_constants::match_default;

        if (!std::regex_search(begin_it, line.cend(), match, regex, flags))
            return std::nullopt;

        const std::size_t match_begin = search_begin
            + static_cast<std::size_t>(match.position());
        const auto match_length = static_cast<std::size_t>(match.length());

        if (match_begin > max_begin) return std::nullopt;

        if (match_length > 0)
            return wide_range(buffer, row, match_begin, match_begin + match_length);

        if (match_begin >= line.size()) return std::nullopt;

        search_begin = match_begin + 1;
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<Syntax::PointRange> last_regex_match_in_line(
    const TextBuffer &buffer,
    const std::wstring &line,
    uint32_t row,
    const std::wregex &regex,
    std::size_t min_begin,
    std::size_t max_end
) {
    if (line.empty() || min_begin > line.size()) {
        return std::nullopt;
    }

    std::optional<Syntax::PointRange> best;

    for (std::size_t search_begin = min_begin; search_begin <= line.size();) {
        std::wsmatch match;
        const auto begin_it = line.cbegin()
            + static_cast<std::ptrdiff_t>(search_begin);
        const auto flags = search_begin > 0
            ? std::regex_constants::match_not_bol
            : std::regex_constants::match_default;

        if (!std::regex_search(begin_it, line.cend(), match, regex, flags))
            break;

        const std::size_t match_begin = search_begin
            + static_cast<std::size_t>(match.position());
        const auto match_length = static_cast<std::size_t>(match.length());
        const std::size_t match_end = match_begin + match_length;

        if (match_end > max_end) break;

        if (match_length > 0) {
            best = wide_range(buffer, row, match_begin, match_end);
        }

        if (match_begin >= line.size()) break;

        search_begin = match_begin + 1;
    }

    return best;
}

[[nodiscard]] std::optional<Syntax::PointRange> first_literal_match_in_line(
    const TextBuffer &buffer,
    std::wstring_view line,
    uint32_t row,
    std::wstring_view pattern,
    std::size_t min_begin,
    std::size_t max_begin
)
{
    if (pattern.empty() || pattern.size() > line.size() || min_begin > line.size()) {
        return std::nullopt;
    }

    const std::size_t last_possible_begin = line.size() - pattern.size();
    if (min_begin > last_possible_begin) {
        return std::nullopt;
    }

    max_begin = std::min(max_begin, last_possible_begin);
    if (min_begin > max_begin) {
        return std::nullopt;
    }

    const std::size_t match_begin = line.find(pattern, min_begin);
    if (match_begin == std::wstring_view::npos || match_begin > max_begin)
        return std::nullopt;

    return wide_range(buffer, row, match_begin, match_begin + pattern.size());
}

[[nodiscard]] std::optional<Syntax::PointRange> last_literal_match_in_line(
    const TextBuffer &buffer,
    std::wstring_view line,
    uint32_t row,
    std::wstring_view pattern,
    std::size_t min_begin,
    std::size_t max_end
) {
    if (pattern.empty() || pattern.size() > line.size() || min_begin > line.size())
        return std::nullopt;

    const std::size_t last_possible_begin = line.size() - pattern.size();
    if (max_end < pattern.size()) {
        return std::nullopt;
    }

    const std::size_t max_begin = std::min(last_possible_begin, max_end - pattern.size());
    if (min_begin > max_begin) {
        return std::nullopt;
    }

    const std::size_t match_begin = line.rfind(pattern, max_begin);
    if (match_begin == std::wstring_view::npos || match_begin < min_begin) {
        return std::nullopt;
    }

    return wide_range(buffer, row, match_begin, match_begin + pattern.size());
}

[[nodiscard]] int normalized_display_cols(int cols) noexcept
{ return std::max(cols, 1); }

[[nodiscard]] DisplayRow display_row_starting_at(
    const TextBuffer &buffer,
    uint32_t row,
    uint32_t start_column,
    int cols
) {
    assert(row < buffer.line_count());

    cols = normalized_display_cols(cols);

    const std::u8string_view line = buffer.line_bytes(row);
    const auto line_size = static_cast<uint32_t>(line.size());

    start_column = utf8::clamp_to_char_boundary(line, start_column);

    if (line.empty()) return DisplayRow{cols, row, 0, 0, 0};

    if (start_column >= line_size)
        return DisplayRow{cols, row, line_size, line_size, 0};

    int width = 0;

    for (uint32_t column = start_column; column < line_size;) {
        const auto decoded = utf8::decode_one(line.substr(column));
        const int char_width = character_width(decoded.wc);

        if (char_width > 0 && width > 0 && width + char_width > cols) {
            return DisplayRow{cols, row, start_column, column, width};
        }

        width += char_width;
        column += decoded.bytes;
    }

    return DisplayRow{cols, row, start_column, line_size, width};
}

[[nodiscard]] DisplayRow last_display_row_for_line(
    const TextBuffer &buffer, uint32_t row, int cols
)
{
    assert(row < buffer.line_count());

    const auto line_size = static_cast<uint32_t>(
        buffer.line_bytes(row).size()
    );
    DisplayRow display_row = display_row_starting_at(buffer, row, 0, cols);

    while (display_row.end_column < line_size) {
        display_row = display_row_starting_at(
            buffer,
            row,
            display_row.end_column,
            cols
        );
    }

    return display_row;
}

[[nodiscard]] DisplayRow display_row_ending_at(
    const TextBuffer &buffer,
    uint32_t row,
    uint32_t end_column,
    int cols
)
{
    assert(row < buffer.line_count());

    const std::u8string_view line = buffer.line_bytes(row);
    end_column = utf8::clamp_to_char_boundary(line, end_column);

    DisplayRow display_row = display_row_starting_at(buffer, row, 0, cols);

    while (display_row.end_column < end_column) {
        display_row = display_row_starting_at(
            buffer,
            row,
            display_row.end_column,
            cols
        );
    }

    return display_row;
}

} // namespace

TextBuffer::TextBuffer()
: line_start_byte{0}
{}

TextBuffer::TextBuffer(const std::filesystem::path &path)
: TextBuffer{}
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::filesystem::filesystem_error{
            "open", path, std::make_error_code(std::errc::io_error)
        };
    }

    std::array<char, 64 * 1024> chunk{};
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > std::numeric_limits<uint32_t>::max() - utf8.size()) {
            throw std::length_error{"file is too large for TextBuffer"};
        }
        utf8.append(reinterpret_cast<const char8_t *>(chunk.data()), count);
    }

    if (!file.eof()) {
        throw std::filesystem::filesystem_error{
            "read", path, std::make_error_code(std::errc::io_error)
        };
    }

    for (std::size_t i = 0; i < utf8.size(); ++i)
        if (utf8[i] == u8'\n')
            line_start_byte.push_back(static_cast<uint32_t>(i + 1));
}

TextBuffer::~TextBuffer() = default;

TextBuffer::TextBuffer(TextBuffer &&other) noexcept = default;

TextBuffer &TextBuffer::operator=(TextBuffer &&other) noexcept = default;

void TextBuffer::swap(TextBuffer &other) noexcept
{
    using std::swap;
    swap(utf8, other.utf8);
    swap(line_start_byte, other.line_start_byte);
    swap(active_language, other.active_language);
    swap(syntax, other.syntax);
    swap(parsing_suspended, other.parsing_suspended);
    swap(pending_syntax_edits, other.pending_syntax_edits);
}

TextBuffer::ReparseSuspension::ReparseSuspension(TextBuffer& text_buffer) noexcept
: buffer{&text_buffer}
{
    if (!buffer->parsing_suspended) {
        buffer->parsing_suspended = true;
        active = true;
    }
}

TextBuffer::ReparseSuspension::~ReparseSuspension() noexcept(false)
{ release(); }

TextBuffer::ReparseSuspension::ReparseSuspension(ReparseSuspension&& other) noexcept
: buffer{std::exchange(other.buffer, nullptr)}
, active{std::exchange(other.active, false)}
{}

TextBuffer::ReparseSuspension&
TextBuffer::ReparseSuspension::operator=(ReparseSuspension&& other) noexcept(false)
{
    if (this != &other) {
        release();
        buffer = std::exchange(other.buffer, nullptr);
        active = std::exchange(other.active, false);
    }

    return *this;
}

void TextBuffer::ReparseSuspension::release()
{
    TextBuffer* text_buffer = std::exchange(buffer, nullptr);
    if (!std::exchange(active, false) || text_buffer == nullptr) {
        return;
    }

    text_buffer->parsing_suspended = false;
    text_buffer->flush_syntax_edits();
}

TextBuffer::ReparseSuspension TextBuffer::suspend_reparse() noexcept
{ return ReparseSuspension{*this}; }

bool TextBuffer::set_language(Syntax::Language lang)
{
    const auto languages = Syntax::available_languages();
    if (std::ranges::find(languages, lang) == languages.end()) {
        return false;
    }

    active_language = lang;
    syntax.reset();
    pending_syntax_edits.clear();
    return true;
}

std::optional<Syntax::Language> TextBuffer::selected_language() const noexcept
{
    return active_language;
}

bool TextBuffer::ensure_syntax() const
{
    if (!syntax) {
        if (!active_language) {
            return false;
        }

        try {
            syntax.emplace(utf8, *active_language);
        } catch (...) {
            return false;
        }

        return static_cast<bool>(*syntax);
    }

    return *syntax || syntax->continue_parsing(utf8);
}

std::optional<Syntax::Node> TextBuffer::syntax_node_at(
    Syntax::Point position, SyntaxNodeMode mode
) const
{
    if (!ensure_syntax()) return std::nullopt;

    position = clamp(position);

    const auto node = syntax_node_for_range(
        *syntax, Syntax::PointRange{position, position}, mode
    );
    return node;
}

std::optional<Syntax::PointRange> TextBuffer::syntax_range_at(
    Syntax::Point point, SyntaxNodeMode mode
) const
{
    const auto node = syntax_node_at(point, mode);
    return node ? syntax_node_range(*node, mode) : std::nullopt;
}

std::optional<Syntax::PointRange> TextBuffer::syntax_expand_range(
    Syntax::PointRange range, SyntaxNodeMode mode
) const
{
    if (!ensure_syntax()) return std::nullopt;

    range = clamp(range);

    const auto node = syntax_node_for_range(*syntax, range, mode);
    if (!node) return std::nullopt;

    const auto parent = nearest_syntax_node(node->parent(), mode);
    if (!parent) return std::nullopt;

    return syntax_node_range(*parent, mode);
}

std::optional<Syntax::PointRange> TextBuffer::syntax_shrink_range(
    Syntax::PointRange range, SyntaxNodeMode mode
) const
{
    if (!ensure_syntax()) return std::nullopt;

    range = clamp(range);

    const auto node = syntax_node_for_range(*syntax, range, mode);
    if (!node) return std::nullopt;

    auto children = mode == SyntaxNodeMode::Named
        ? node->named_children()
        : node->children();

    for (const auto child : children) {
        if (const auto child_range = syntax_node_range(child, mode))
            return child_range;
    }

    return std::nullopt;
}

std::optional<Syntax::PointRange> TextBuffer::syntax_next_range(
    Syntax::PointRange range, SyntaxNodeMode mode
) const
{
    if (!ensure_syntax()) {
        return std::nullopt;
    }

    range = clamp(range);

    const auto node = syntax_node_for_range(*syntax, range, mode);
    if (!node) {
        return std::nullopt;
    }

    if (const auto sibling = next_syntax_sibling(*node, mode)) {
        return syntax_node_range(*sibling, mode);
    }

    return std::nullopt;
}

std::optional<Syntax::PointRange> TextBuffer::syntax_previous_range(
    Syntax::PointRange range, SyntaxNodeMode mode
) const
{
    if (!ensure_syntax()) return std::nullopt;

    range = clamp(range);

    const auto node = syntax_node_for_range(*syntax, range, mode);
    if (!node) return std::nullopt;

    if (const auto sibling = previous_syntax_sibling(*node, mode))
        return syntax_node_range(*sibling, mode);

    return std::nullopt;
}

uint32_t TextBuffer::line_count() const
{ return static_cast<uint32_t>(line_start_byte.size()); }

std::u8string_view TextBuffer::bytes() const noexcept
{ return utf8; }

std::u8string_view TextBuffer::line_bytes(uint32_t row) const noexcept
{
    const auto begin = line_start_byte[row];
    const auto end = line_end_byte(row);

    return std::u8string_view{utf8}.substr(begin, end - begin);
}

std::wstring TextBuffer::line_wide(uint32_t row) const
{ return utf8::decode(line_bytes(row)); }

Syntax::Point TextBuffer::line_end(uint32_t row) const
{
    return {
        .row = row,
        .byte_column = static_cast<uint32_t>(line_bytes(row).size()),
    };
}

Syntax::Point TextBuffer::clamp(Syntax::Point p) const
{
    if (p.row >= line_count()) return end();

    p.byte_column = utf8::clamp_to_char_boundary(line_bytes(p.row), p.byte_column);
    return p;
}

Syntax::PointRange TextBuffer::clamp(Syntax::PointRange range) const
{
    range = Syntax::PointRange{clamp(range.start), clamp(range.end)};
    if (range.end < range.start) std::swap(range.start, range.end);
    return range;
}

uint32_t TextBuffer::byte_offset(Syntax::Point p) const
{
    p = clamp(p);
    return line_start_byte[p.row] + p.byte_column;
}

Syntax::Point TextBuffer::position_at_byte(uint32_t byte) const
{
    byte = std::min<decltype(byte)>(byte, utf8.size());

    // Find the last line start that is <= byte.
    const auto row_it = std::prev(
        std::ranges::upper_bound(line_start_byte, byte)
    );

    return {
        .row = static_cast<uint32_t>(row_it - line_start_byte.begin()),
        .byte_column = byte - *row_it
    };
}

std::optional<wchar_t> TextBuffer::char_at(Syntax::Point p) const
{
    const uint32_t byte = byte_offset(p);

    if (byte >= utf8.size()) {
        return std::nullopt;
    }

    return utf8::decode_one(bytes().substr(byte)).wc;
}

std::optional<Syntax::Point> TextBuffer::next_position(Syntax::Point p) const
{
    const uint32_t byte = byte_offset(p);

    if (byte >= utf8.size()) return std::nullopt;

    const auto decoded = utf8::decode_one(bytes().substr(byte));
    return position_at_byte(byte + decoded.bytes);
}

std::optional<Syntax::Point> TextBuffer::previous_position(Syntax::Point p) const
{
    const uint32_t byte = byte_offset(p);

    if (byte == 0) {
        return std::nullopt;
    }

    return position_at_byte(utf8::previous_char_start(utf8, byte));
}

std::optional<Syntax::PointRange> TextBuffer::word_range_at(
    Syntax::Point position, bool find_forward
) const
{
    static const std::wregex keyword_character(LR"([[:alnum:]_])");
    return word_range_at(position, keyword_character, find_forward);
}

std::optional<Syntax::PointRange> TextBuffer::word_range_at(
    Syntax::Point point, const std::wregex &character_regex, bool find_forward
) const
{
    point = clamp(point);
    const std::wstring line = line_wide(point.row);
    std::size_t column = wide_column(point);

    const auto matches = [&](wchar_t wc) {
        const wchar_t character[]{wc, L'\0'};
        return std::regex_match(character, character_regex);
    };

    if (find_forward) {
        while (column < line.size() && !matches(line[column])) {
            ++column;
        }
    }

    if (column >= line.size() || !matches(line[column])) return std::nullopt;

    std::size_t begin = column;
    while (begin > 0 && matches(line[begin - 1])) --begin;

    std::size_t end = column + 1;
    while (end < line.size() && matches(line[end])) ++end;

    return wide_range(*this, point.row, begin, end);
}

std::optional<Syntax::PointRange> TextBuffer::find_regex(
    const std::wregex &regex, Direction direction, Syntax::Point origin
) const
{
    origin = clamp(origin);

    const uint32_t origin_row = origin.row;
    const std::size_t origin_column = wide_column(origin);

    if (direction == Direction::Forward) {
        std::size_t column = origin_column;

        for (uint32_t row = origin_row; row < line_count(); ++row) {
            const std::wstring line = line_wide(row);

            if (row == origin_row && column < line.size()) {
                ++column;
            }

            if (auto match = first_regex_match_in_line(
                    *this,
                    line,
                    row,
                    regex,
                    column,
                    line.size()
                )) {
                return match;
            }

            column = 0;
        }

        for (uint32_t row = 0; row <= origin_row && row < line_count(); ++row) {
            const std::wstring line = line_wide(row);
            std::size_t max_begin = line.size();
            if (row == origin_row) {
                if (origin_column == 0) {
                    continue;
                }
                max_begin = origin_column - 1;
            }

            if (auto match = first_regex_match_in_line(
                    *this,
                    line,
                    row,
                    regex,
                    0,
                    max_begin
                )) {
                return match;
            }
        }

        return std::nullopt;
    }

    for (uint32_t row = origin_row + 1; row > 0;) {
        --row;

        const std::wstring line = line_wide(row);
        const std::size_t max_end = row == origin_row
            ? origin_column
            : line.size();

        if (auto match = last_regex_match_in_line(
                *this,
                line,
                row,
                regex,
                0,
                max_end
            )) {
            return match;
        }
    }

    for (uint32_t row = line_count(); row > origin_row + 1;) {
        --row;

        const std::wstring line = line_wide(row);
        if (auto match = last_regex_match_in_line(
                *this,
                line,
                row,
                regex,
                0,
                line.size()
            )) {
            return match;
        }
    }

    const std::wstring origin_line = line_wide(origin_row);
    const std::size_t min_begin = origin_column + 1;
    if (min_begin > origin_line.size()) {
        return std::nullopt;
    }
    return last_regex_match_in_line(
        *this,
        origin_line,
        origin_row,
        regex,
        min_begin,
        origin_line.size()
    );
}

std::optional<Syntax::PointRange> TextBuffer::find_literal(
    std::wstring_view pattern,
    Direction direction,
    Syntax::Point origin,
    bool include_origin,
    bool line_local
) const
{
    if (pattern.empty()) {
        return std::nullopt;
    }

    origin = clamp(origin);

    const uint32_t origin_row = origin.row;
    const std::size_t origin_column = wide_column(origin);

    if (line_local) {
        const std::wstring line = line_wide(origin_row);

        if (direction == Direction::Forward) {
            std::size_t column = origin_column;
            if (!include_origin && column < line.size()) {
                ++column;
            }

            return first_literal_match_in_line(
                *this,
                line,
                origin_row,
                pattern,
                column,
                line.size()
            );
        }

        return last_literal_match_in_line(
            *this,
            line,
            origin_row,
            pattern,
            0,
            origin_column
        );
    }

    if (direction == Direction::Forward) {
        std::size_t column = origin_column;

        for (uint32_t row = origin_row; row < line_count(); ++row) {
            const std::wstring line = line_wide(row);

            if (row == origin_row && !include_origin && column < line.size()) {
                ++column;
            }

            if (auto match = first_literal_match_in_line(
                    *this,
                    line,
                    row,
                    pattern,
                    column,
                    line.size()
                )) {
                return match;
            }

            column = 0;
        }

        for (uint32_t row = 0; row <= origin_row && row < line_count(); ++row) {
            const std::wstring line = line_wide(row);

            if (row == origin_row && include_origin && origin_column == 0) {
                continue;
            }

            const std::size_t max_begin = row == origin_row
                ? (include_origin ? origin_column - 1 : origin_column)
                : line.size();

            if (auto match = first_literal_match_in_line(
                    *this,
                    line,
                    row,
                    pattern,
                    0,
                    max_begin
                )) {
                return match;
            }
        }

        return std::nullopt;
    }

    for (uint32_t row = origin_row + 1; row > 0;) {
        --row;

        const std::wstring line = line_wide(row);
        const std::size_t max_end = row == origin_row
            ? origin_column
            : line.size();

        if (auto match = last_literal_match_in_line(
                *this,
                line,
                row,
                pattern,
                0,
                max_end
            )) {
            return match;
        }
    }

    for (uint32_t row = line_count(); row > origin_row + 1;) {
        --row;

        const std::wstring line = line_wide(row);
        if (auto match = last_literal_match_in_line(
                *this,
                line,
                row,
                pattern,
                0,
                line.size()
            )) {
            return match;
        }
    }

    const std::wstring origin_line = line_wide(origin_row);
    return last_literal_match_in_line(
        *this,
        origin_line,
        origin_row,
        pattern,
        origin_column,
        origin_line.size()
    );
}

int TextBuffer::display_width(
    uint32_t row,
    uint32_t begin_column,
    uint32_t end_column
) const
{
    const std::u8string_view line = line_bytes(row);

    begin_column = utf8::clamp_to_char_boundary(line, begin_column);
    end_column = utf8::clamp_to_char_boundary(line, end_column);

    if (end_column < begin_column) {
        std::swap(begin_column, end_column);
    }

    int width = 0;

    for (uint32_t column = begin_column; column < end_column;) {
        const auto decoded = utf8::decode_one(line.substr(column));
        width += character_width(decoded.wc);
        column += decoded.bytes;
    }

    return width;
}

int TextBuffer::line_display_width(uint32_t row) const
{
    return display_width(row, 0, line_bytes(row).size());
}

DisplayRow TextBuffer::display_row_at(Syntax::Point point, int cols) const
{
    point = clamp(point);

    const auto line_size = static_cast<uint32_t>(line_bytes(point.row).size());
    DisplayRow display_row = display_row_starting_at(
        *this,
        point.row,
        0,
        cols
    );

    while (
        point.byte_column >= display_row.end_column
        && display_row.end_column < line_size
    ) {
        display_row = display_row_starting_at(
            *this,
            point.row,
            display_row.end_column,
            cols
        );
    }

    return display_row;
}

std::optional<DisplayRow> TextBuffer::next_display_row(
    DisplayRow display_row
) const
{
    if (display_row.row >= line_count()) {
        return std::nullopt;
    }

    const int cols = normalized_display_cols(display_row.cols);
    const uint32_t end_column = clamp(
        Syntax::Point{display_row.row, display_row.end_column}
    ).byte_column;
    const auto line_size = static_cast<uint32_t>(
        line_bytes(display_row.row).size()
    );

    if (end_column < line_size) {
        return display_row_starting_at(
            *this,
            display_row.row,
            end_column,
            cols
        );
    }

    if (display_row.row < line_count() - 1) {
        return display_row_starting_at(*this, display_row.row + 1, 0, cols);
    }

    return std::nullopt;
}

std::optional<DisplayRow> TextBuffer::previous_display_row(
    DisplayRow display_row
) const
{
    if (display_row.row >= line_count()) {
        return std::nullopt;
    }

    const int cols = normalized_display_cols(display_row.cols);
    const uint32_t begin_column = clamp(
        Syntax::Point{display_row.row, display_row.begin_column}
    ).byte_column;

    if (begin_column > 0) {
        return display_row_ending_at(*this, display_row.row, begin_column, cols);
    }

    if (display_row.row > 0) {
        return last_display_row_for_line(*this, display_row.row - 1, cols);
    }

    return std::nullopt;
}

std::u8string TextBuffer::extract_utf8(Syntax::PointRange range) const
{
    range = clamp(range);

    const uint32_t begin = byte_offset(range.start);
    const uint32_t end = byte_offset(range.end);

    return utf8.substr(begin, end - begin);
}

std::wstring TextBuffer::extract_wide(Syntax::PointRange range) const
{ return utf8::decode(extract_utf8(range)); }

std::optional<std::string> TextBuffer::extract_multibyte(Syntax::PointRange range) const
{
    const std::wstring wide = extract_wide(range);
    const std::size_t length = std::wcstombs(nullptr, wide.c_str(), 0);

    if (length == static_cast<std::size_t>(-1)) {
        return std::nullopt;
    }

    std::string out(length, '\0');
    if (length == 0) {
        return out;
    }

    if (std::wcstombs(out.data(), wide.c_str(), out.size()) == static_cast<std::size_t>(-1)) {
        return std::nullopt;
    }

    return out;
}

std::size_t TextBuffer::wide_column(Syntax::Point p) const
{
    p = clamp(p);

    const std::u8string_view line = line_bytes(p.row);

    uint32_t column = 0;
    std::size_t index = 0;

    while (column < p.byte_column) {
        const auto decoded = utf8::decode_one(line.substr(column));
        column += decoded.bytes;
        ++index;
    }

    return index;
}

uint32_t TextBuffer::byte_column_for_wide_index(
    uint32_t row,
    std::size_t wide_index
) const {
    const std::u8string_view line = line_bytes(row);

    uint32_t column = 0;
    std::size_t index = 0;

    while (column < line.size() && index < wide_index) {
        const auto decoded = utf8::decode_one(line.substr(column));
        column += decoded.bytes;
        ++index;
    }

    return column;
}

void TextBuffer::append(wchar_t wc)
{
    if (wc == L'\n') {
        insert_at_end(u8"\n");
        return;
    }

    if (wc == L'\r') {
        return;
    }

    if (wc == L'\b' || wc == static_cast<wchar_t>(0x7f)) {
        erase_previous_char();
        return;
    }

    if (wc == L'\t') {
        const int col = line_display_width(last_row()) % 8;
        const int spaces = 8 - col;

        std::u8string expanded(spaces, u8' ');
        insert_at_end(expanded);
        return;
    }

    if ((wc < 32) || (wc >= 0x7f && wc < 0xa0)) {
        return;
    }

    if (wcwidth(wc) < 0) {
        wc = replacement_character;
    }

    std::u8string encoded;
    utf8::append(encoded, static_cast<char32_t>(wc));
    insert_at_end(encoded);
}

void TextBuffer::append_ascii(std::string_view text)
{ for (unsigned char ch : text) append(static_cast<wchar_t>(ch)); }

uint32_t TextBuffer::line_end_byte(uint32_t row) const
{
    if (auto next_row = row + 1; next_row < line_start_byte.size()) {
        return line_start_byte[next_row] - 1; // exclude '\n'
    }

    return static_cast<uint32_t>(utf8.size());
}

uint32_t TextBuffer::last_row() const noexcept
{ return static_cast<uint32_t>(line_start_byte.size()) - 1; }

void TextBuffer::apply_syntax_edit(Syntax::Edit edit)
{
    if (!syntax) {
        return;
    }

    if (parsing_suspended) {
        pending_syntax_edits.push_back(edit);
        return;
    }

    (void)syntax->edit(utf8, edit);
}

void TextBuffer::flush_syntax_edits()
{
    if (!syntax || pending_syntax_edits.empty()) {
        pending_syntax_edits.clear();
        return;
    }

    (void)syntax->edit(utf8, pending_syntax_edits);
    pending_syntax_edits.clear();
}

void TextBuffer::insert_at_end(std::u8string_view text)
{
    if (text.empty()) return;

    const auto start_byte = static_cast<uint32_t>(utf8.size());
    const Syntax::Point start_point{
        last_row(),
        start_byte - line_start_byte.back(),
    };
    const auto new_end_byte = start_byte + static_cast<uint32_t>(text.size());
    const Syntax::Point new_end_point = advance_point(start_point, text);

    const Syntax::Edit edit{
        .old = {
            .start = syntax_position(start_byte, start_point),
            .end = syntax_position(start_byte, start_point),
        },
        .new_end = syntax_position(new_end_byte, new_end_point),
    };

    utf8.append(text);

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == u8'\n') {
            line_start_byte.push_back(start_byte + static_cast<uint32_t>(i) + 1);
        }
    }

    apply_syntax_edit(edit);
}

void TextBuffer::erase_previous_char()
{
    if (utf8.empty()) return;

    const auto old_end_byte = static_cast<uint32_t>(utf8.size());
    const auto start_byte = utf8::previous_char_start(utf8, old_end_byte);
    const auto start_point = position_at_byte(start_byte);
    const auto old_end_point = position_at_byte(old_end_byte);

    const Syntax::Edit edit{
        .old = {
            .start = syntax_position(start_byte, start_point),
            .end = syntax_position(old_end_byte, old_end_point),
        },
        .new_end = syntax_position(start_byte, start_point),
    };

    const bool erased_newline = utf8[start_byte] == u8'\n';

    utf8.erase(start_byte);

    if (erased_newline) {
        line_start_byte.pop_back();
    }

    apply_syntax_edit(edit);
}

} // namespace mlang
