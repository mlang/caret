#pragma once

#include <mlang/syntax.hpp>

#include <compare>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mlang {

struct DisplayRow
{
    int cols = 1;
    uint32_t row = 0;
    uint32_t begin_column = 0;
    uint32_t end_column = 0;
    int width = 0;

    Syntax::Point begin() const noexcept { return {row, begin_column}; }
    Syntax::Point end() const noexcept { return {row, end_column}; }

    [[nodiscard]] Syntax::PointRange range() const noexcept
    { return {begin(), end()}; }

    [[nodiscard]] friend auto operator<=>(DisplayRow lhs, DisplayRow rhs) noexcept
    { return lhs.begin() <=> rhs.begin(); }

    [[nodiscard]] friend bool operator==(DisplayRow lhs, DisplayRow rhs) noexcept
    { return lhs.begin() == rhs.begin(); }
};

constexpr wchar_t replacement_character = 0xfffd;

enum class SyntaxNodeMode { Named, Any };

enum class Direction { Forward, Backward };

class TextBuffer
{
    std::u8string utf8;
    std::vector<uint32_t> line_start_byte;
    std::optional<Syntax::Language> active_language;
    mutable std::optional<Syntax> syntax;
    bool parsing_suspended = false;
    std::vector<Syntax::Edit> pending_edits;

    [[nodiscard]] uint32_t line_end_byte(uint32_t row) const;
    [[nodiscard]] uint32_t last_row() const noexcept;
    [[nodiscard]] bool ensure_syntax() const;

    void apply_syntax_edit(Syntax::Edit edit);
    void flush_syntax_edits();
    void insert_at_end(std::u8string_view text);
    void erase_previous_char();

public:
    TextBuffer();
    explicit TextBuffer(const std::filesystem::path &path);
    ~TextBuffer();

    TextBuffer(const TextBuffer &) = delete;
    TextBuffer &operator=(const TextBuffer &) = delete;

    TextBuffer(TextBuffer &&other) noexcept;
    TextBuffer &operator=(TextBuffer &&other) noexcept;

    void swap(TextBuffer &other) noexcept;

    class ReparseSuspension
    {
        friend class TextBuffer;

        TextBuffer* buffer = nullptr;
        bool active = false;

        explicit ReparseSuspension(TextBuffer&) noexcept;
        void release();

    public:
        ~ReparseSuspension() noexcept(false);

        ReparseSuspension(const ReparseSuspension&) = delete;
        ReparseSuspension& operator=(const ReparseSuspension&) = delete;

        ReparseSuspension(ReparseSuspension&&) noexcept;
        ReparseSuspension& operator=(ReparseSuspension&&) noexcept(false);
    };

    [[nodiscard]] ReparseSuspension suspend_reparse() noexcept;

    bool set_language(Syntax::Language);

    [[nodiscard]] std::optional<Syntax::Language> selected_language() const noexcept;

    [[nodiscard]] std::optional<Syntax::Node> syntax_node_at(
        Syntax::Point point, SyntaxNodeMode mode = SyntaxNodeMode::Named
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> syntax_range_at(
        Syntax::Point point, SyntaxNodeMode mode = SyntaxNodeMode::Named
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> syntax_expand_range(
        Syntax::PointRange range, SyntaxNodeMode mode = SyntaxNodeMode::Named
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> syntax_shrink_range(
        Syntax::PointRange range, SyntaxNodeMode mode = SyntaxNodeMode::Named
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> syntax_next_range(
        Syntax::PointRange range, SyntaxNodeMode mode = SyntaxNodeMode::Named
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> syntax_previous_range(
        Syntax::PointRange range, SyntaxNodeMode mode = SyntaxNodeMode::Named
    ) const;

    [[nodiscard]] std::u8string_view bytes() const noexcept;
    [[nodiscard]] uint32_t line_count() const;
    [[nodiscard]] std::u8string_view line_bytes(uint32_t row) const noexcept;
    [[nodiscard]] std::wstring line_wide(uint32_t row) const;
    [[nodiscard]] Syntax::Point line_end(uint32_t row) const;

    [[nodiscard]] Syntax::Point end() const { return line_end(last_row()); }

    [[nodiscard]] Syntax::Point clamp(Syntax::Point) const;
    [[nodiscard]] Syntax::PointRange clamp(Syntax::PointRange range) const;

    [[nodiscard]] uint32_t byte_offset(Syntax::Point p) const;
    [[nodiscard]] Syntax::Point point_at_byte(uint32_t byte) const;

    [[nodiscard]] std::optional<wchar_t> char_at(Syntax::Point p) const;
    [[nodiscard]] std::optional<Syntax::Point> next_position(Syntax::Point) const;
    [[nodiscard]] std::optional<Syntax::Point> previous_position(Syntax::Point) const;

    [[nodiscard]] std::optional<Syntax::PointRange> word_range_at(
        Syntax::Point, bool find_forward = false
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> word_range_at(
        Syntax::Point,
        const std::wregex &character_regex,
        bool find_forward = false
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> find_regex(
        const std::wregex &regex,
        Direction direction,
        Syntax::Point origin
    ) const;

    [[nodiscard]] std::optional<Syntax::PointRange> find_literal(
        std::wstring_view pattern,
        Direction direction,
        Syntax::Point origin,
        bool include_origin = false,
        bool line_local = false
    ) const;

    [[nodiscard]] int display_width(
        uint32_t row,
        uint32_t begin_column,
        uint32_t end_column
    ) const;

    [[nodiscard]] int line_display_width(uint32_t row) const;

    [[nodiscard]] DisplayRow display_row_at(Syntax::Point, int cols) const;
    [[nodiscard]] std::optional<DisplayRow> next_display_row(DisplayRow row) const;
    [[nodiscard]] std::optional<DisplayRow> previous_display_row(DisplayRow row) const;

    [[nodiscard]] std::u8string extract_utf8(Syntax::PointRange range) const;
    [[nodiscard]] std::wstring extract_wide(Syntax::PointRange range) const;
    [[nodiscard]] std::optional<std::string> extract_multibyte(Syntax::PointRange range) const;

    // Handy when std::wregex gives match offsets in decoded wchar_t units.
    std::size_t wide_column(Syntax::Point) const;
    uint32_t byte_column_for_wide_index(uint32_t row, std::size_t wide_index) const;

    void append(wchar_t wc);
    void append_ascii(std::string_view text);
};

} // namespace
