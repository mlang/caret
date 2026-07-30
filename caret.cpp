#include <algorithm>
#include <mlang/curses.hpp>
#include <mlang/text_buffer.hpp>

#include <array>
#include <cassert>
#include <cerrno>
#include <climits>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <numeric>
#include <optional>
#include <poll.h>
#include <print>
#include <regex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

using mlang::replacement_character, mlang::Syntax, mlang::TextBuffer, mlang::DisplayRow, mlang::Direction;

namespace {

constexpr std::wstring_view word_separators = LR"sep(!"#$%&'()*+,-./:;<=>?@[\]^`{|}~)sep";

constexpr std::array<std::wstring_view, 2> bracket_chars{ L"({[", L")}]" };

std::wstring escape_regex(std::wstring_view literal);

[[noreturn]] void fatal(const char *message)
{
    throw std::system_error(errno, std::generic_category(), message);
}

class SigpipeGuard
{
    struct sigaction old_action {};
    bool active = false;

public:
    SigpipeGuard()
    {
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        active = sigaction(SIGPIPE, &ignore, &old_action) == 0;
    }

    SigpipeGuard(const SigpipeGuard &) = delete;
    SigpipeGuard &operator=(const SigpipeGuard &) = delete;

    ~SigpipeGuard()
    { if (active) (void)sigaction(SIGPIPE, &old_action, nullptr); }
};

std::expected<void, std::error_code>
write_all(int fd, std::string_view s)
{
    while (!s.empty()) {
        const ssize_t n = write(fd, s.data(), s.size());

        if (n > 0) {
            s.remove_prefix(n);
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        return std::unexpected(std::error_code{
            n < 0 ? errno : EIO, std::generic_category()
        });
    }

    return {};
}

void write_all_or_throw(int fd, std::string_view bytes)
{
    if (auto result = write_all(fd, bytes); !result)
        throw std::system_error(result.error(), "write");
}

void write_all_no_throw(int fd, std::string_view bytes)
{
    SigpipeGuard guard;
    (void)write_all(fd, bytes);
}

enum class ChildOutput { inherit, discard };

int pipe_to(
    std::string program,
    std::vector<std::string> args,
    std::string_view input,
    ChildOutput output = ChildOutput::inherit
)
{
    std::vector<char *> argv;
    argv.reserve(args.size() + 2);

    argv.push_back(program.data());
    for (auto &arg: args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    int fds[2] = {-1, -1};
    if (pipe(fds) < 0)
        throw std::system_error(errno, std::generic_category(), "pipe");

    const pid_t pid = fork();
    if (pid < 0) {
        const int e = errno;
        close(fds[0]);
        close(fds[1]);
        throw std::system_error(e, std::generic_category(), "fork");
    }

    if (pid == 0) {
        close(fds[1]);

        if (dup2(fds[0], STDIN_FILENO) < 0) _exit(127);
        close(fds[0]);

        if (output == ChildOutput::discard) {
            const int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                if (dup2(dev_null, STDOUT_FILENO) < 0) _exit(127);
                if (dup2(dev_null, STDERR_FILENO) < 0) _exit(127);
                close(dev_null);
            }
        }

        execvp(program.c_str(), argv.data());
        _exit(127);
    }

    close(fds[0]);

    std::error_code write_error;
    {
        SigpipeGuard guard;
        if (auto result = write_all(fds[1], input); !result) {
            write_error = result.error();
        }
    }

    close(fds[1]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "waitpid");

    // Normal if the child exits without reading all input.
    if (write_error  && write_error.value() != EPIPE)
        throw std::system_error(write_error, "write");

    return status;
}

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ParsedArguments
{
    struct InputSpec
    {
        std::filesystem::path path;
        std::optional<Syntax::Language> language;
    };

    bool list_languages = false;
    std::optional<Syntax::Language> stdin_language;
    std::vector<InputSpec> inputs;
};

Syntax::Language parse_language(std::string_view name)
{
    if (const auto language = Syntax::language_from(name)) return *language;

    throw UsageError(std::format("unsupported language: {}", name));
}

ParsedArguments parse_arguments(int argc, char **argv)
{
    ParsedArguments parsed;
    std::optional<Syntax::Language> active_language;

    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];

        if (arg == "--") {
            while (++i < argc) {
                parsed.inputs.push_back({argv[i], active_language});
            }
            break;
        }

        if (arg == "-l" || arg == "--language") {
            if (++i >= argc) {
                throw UsageError("missing language after " + std::string(arg));
            }
            active_language = parse_language(argv[i]);
            continue;
        }

        if (arg == "-L" || arg == "--list-languages") {
            parsed.list_languages = true;
            continue;
        }

        constexpr std::string_view language_prefix = "--language=";
        if (arg.starts_with(language_prefix)) {
            active_language = parse_language(arg.substr(language_prefix.size()));
            continue;
        }

        parsed.inputs.push_back({argv[i], active_language});
    }

    parsed.stdin_language = active_language;

    return parsed;
}

void language_listing(std::FILE* out)
{
    for (auto language: Syntax::available_languages())
        std::println(out, "{}", canonical_name(language));
}

Direction opposite(Direction direction)
{
    using enum Direction;
    return direction == Forward ? Backward : Forward;
}

struct ScreenMetrics
{
    int rows = 1;
    int cols = 1;
    int content_rows = 1;

    static ScreenMetrics current()
    {
        ScreenMetrics screen;
        getmaxyx(stdscr, screen.rows, screen.cols);
        screen.rows = std::max(screen.rows, 1);
        screen.cols = std::max(screen.cols, 1);
        screen.content_rows = screen.rows > 1 ? screen.rows - 1 : 1;
        return screen;
    }
};

struct Prompt
{
    bool active = false;
    wchar_t leader = L':';
    std::wstring text;
    size_t cursor = 0;
};

struct PromptHistory
{
    std::optional<size_t> index;
    std::wstring draft;
    size_t draft_cursor = 0;

    void reset() {
        index.reset();
        draft.clear();
        draft_cursor = 0;
    }

    void stop_navigation() {
        if (!index) return;

        index.reset();
        draft.clear();
    }
};

struct SearchPrompt
{
    Direction direction = Direction::Forward;
    PromptHistory history;
};

struct IncrementalSearch
{
    bool active = false;
    Direction direction = Direction::Forward;
    std::wstring pattern;
    Syntax::Point restore_cursor;
    std::optional<Syntax::Point> restore_mark;
    Syntax::Point search_origin;
    std::optional<Syntax::PointRange> match;
    bool failed = false;
};

struct SearchState
{
    SearchPrompt prompt;
    IncrementalSearch incremental;
    std::vector<std::wstring> history;
    std::optional<std::wregex> last_regex;
    Direction last_direction = Direction::Forward;
};

struct CommandState
{
    PromptHistory prompt;
    std::vector<std::wstring> history;
};

using namespace mlang::curses;

enum class InputMode { normal, search_prompt, command_prompt, isearch };
enum class ViewportAlignment { top, middle, bottom };

class Decoder
{
    std::mbstate_t mb{};
    enum class AnsiState { normal, esc, csi, osc, osc_esc } ansi = AnsiState::normal;

    void emit_byte(TextBuffer &doc, unsigned char byte);

public:
    void process_byte(TextBuffer &doc, unsigned char byte);
    void flush(TextBuffer &doc);
};

class Terminal
{
    std::unique_ptr<std::FILE, int (*)(std::FILE *)> tty_in, tty_out;
    SCREEN *screen = nullptr;
    std::optional<termios> original_tty_attrs;

public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal &) = delete;
    Terminal &operator=(const Terminal &) = delete;

    [[nodiscard]] int input_fd() const { return fileno(tty_in.get()); }
    void disable_flow_control();
};

Terminal::Terminal()
: tty_in{fopen("/dev/tty", "r"), fclose}
, tty_out{fopen("/dev/tty", "w"), fclose}
{
    if (!tty_in || !tty_out) fatal("open /dev/tty");

    termios attrs{};
    if (tcgetattr(fileno(tty_in.get()), &attrs) < 0) {
        fatal("tcgetattr");
    }
    original_tty_attrs = attrs;

    screen = newterm(nullptr, tty_out.get(), tty_in.get());
    if (screen == nullptr) {
        throw std::runtime_error("failed to initialize terminal");
    }

    set_term(screen);
}

Terminal::~Terminal()
{
    if (screen != nullptr) {
        endwin();
        delscreen(screen);
    }
    if (original_tty_attrs && tty_in)
        (void)tcsetattr(fileno(tty_in.get()), TCSANOW, &*original_tty_attrs);
}

void Terminal::disable_flow_control()
{
    termios attrs{};
    if (tcgetattr(input_fd(), &attrs) < 0) {
        fatal("tcgetattr");
    }
    attrs.c_iflag &= ~IXON;
    if (tcsetattr(input_fd(), TCSANOW, &attrs) < 0) {
        fatal("tcsetattr");
    }
}

int set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) fatal("fcntl");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) fatal("fcntl");
    return flags;
}

int wchar_width(wchar_t wc)
{
    const int width = wcwidth(wc);
    return width < 0 ? 1 : width;
}

int display_width(std::wstring_view text)
{
    return std::transform_reduce(
        text.begin(), text.end(), 0, std::plus{}, wchar_width
    );
}

int clamp_col_for_row(int display_col, int row_width, int cols)
{
    const int max_col = std::max(0, std::min(row_width, cols - 1));
    return std::clamp(display_col, 0, max_col);
}

int display_col_for_position(const TextBuffer &doc, const DisplayRow &row, Syntax::Point point)
{
    point = doc.clamp(point);
    assert(row.row < doc.line_count());
    assert(row.begin_column <= row.end_column);
    assert(row.end_column <= doc.line_bytes(row.row).size());
    assert(row.row == point.row);
    assert(row.begin_column <= point.byte_column);
    assert(point.byte_column <= row.end_column);

    return doc.display_width(row.row, row.begin_column, point.byte_column);
}

int cursor_display_col(const TextBuffer &doc, const DisplayRow &row, Syntax::Point point, int cols)
{
    return clamp_col_for_row(display_col_for_position(doc, row, point), row.width, cols);
}

Syntax::Point point_for_display_col(const TextBuffer &doc, const DisplayRow &row, int display_col, int cols)
{
    assert(row.row < doc.line_count());
    assert(row.begin_column <= row.end_column);
    assert(row.end_column <= doc.line_bytes(row.row).size());

    display_col = clamp_col_for_row(display_col, row.width, cols);
    const std::wstring line = doc.line_wide(row.row);
    const size_t begin = doc.wide_column({row.row, row.begin_column});
    const size_t end = doc.wide_column({row.row, row.end_column});
    int col = 0;

    for (size_t i = begin; i < end; i++) {
        const int width = wchar_width(line[i]);
        if (display_col <= col) {
            return {row.row, doc.byte_column_for_wide_index(row.row, i)};
        }
        if (display_col < col + width) {
            return {row.row, doc.byte_column_for_wide_index(row.row, i + 1)};
        }
        col += width;
    }

    return {row.row, row.end_column};
}

Syntax::PointRange ordered_range(Syntax::Point start, Syntax::Point end) noexcept
{
    Syntax::PointRange range{start, end};
    if (range.end < range.start) std::swap(range.start, range.end);
    return range;
}

std::wstring decode_wide_string(std::string_view bytes)
{
    std::wstring decoded;
    std::mbstate_t state{};
    const char *data = bytes.data();
    size_t remaining = bytes.size();

    while (remaining > 0) {
        wchar_t wc = 0;
        const size_t n = std::mbrtowc(&wc, data, remaining, &state);
        if (n == static_cast<size_t>(-1)) {
            decoded.push_back(replacement_character);
            data++;
            remaining--;
            state = std::mbstate_t{};
            continue;
        }
        if (n == static_cast<size_t>(-2)) {
            decoded.push_back(replacement_character);
            break;
        }
        if (n == 0) {
            data++;
            remaining--;
            state = std::mbstate_t{};
            continue;
        }
        decoded.push_back(wc);
        data += n;
        remaining -= n;
    }

    return decoded;
}

std::wstring status_position(const TextBuffer &doc, Syntax::Point point)
{
    return std::format(L"{}:{}", point.row + 1, doc.wide_column(point) + 1);
}

std::wstring status_range(
    const TextBuffer &doc, Syntax::Point mark, Syntax::Point cursor
)
{
    const auto range = ordered_range(mark, cursor);
    return std::format(L"{}-{}",
        status_position(doc, range.start), status_position(doc, range.end)
    );
}

void append_stream_char(TextBuffer &doc, wchar_t wc)
{
    if ((wc == L'\b' || wc == static_cast<wchar_t>(0x7f))
        && doc.end().byte_column == 0) {
        return;
    }

    doc.append(wc);
}

void load_tmux_buffer(std::string_view bytes)
{
    const char *tmux = std::getenv("TMUX");
    if (tmux == nullptr || tmux[0] == '\0') return;

    try {
        pipe_to("tmux", {"load-buffer", "-"}, bytes, ChildOutput::discard);
    } catch (...) {
    }
}

void load_brltty_clipboard(std::string_view bytes)
{
    try {
        pipe_to("brltty-clip", {"-"}, bytes, ChildOutput::discard);
    } catch (...) {
    }
}

int document_percent(const TextBuffer &doc, Syntax::Point point)
{
    const std::uint64_t total = doc.line_count();
    const std::uint64_t current = std::uint64_t(point.row) + 1;

    if (current >= total) return 100;

    return (current * 100) / total;
}

struct StatusLine
{
    std::wstring left;
    std::wstring center;
    std::wstring right;
};

std::wstring status_syntax(const TextBuffer &doc, Syntax::Point point)
{
    std::wstring text;

    if (const auto language = doc.selected_language()) {
        text += decode_wide_string(canonical_name(*language));
    }

    if (const auto node = doc.syntax_node_at(point)) {
	text = std::format(L"{}::{}",
            decode_wide_string(canonical_name(node->language())),
            decode_wide_string(node->type())
        );
    }

    return text;
}

std::wstring status_location(
    const TextBuffer &doc, Syntax::Point cursor, const std::optional<Syntax::Point> &mark
)
{
    return mark
         ? status_range(doc, *mark, cursor)
         : status_position(doc, cursor);
}

StatusLine status_line(
    const TextBuffer &buffer,
    Syntax::Point cursor,
    const std::optional<Syntax::Point> &mark,
    std::string_view source
)
{
    return {
        decode_wide_string(source),
        status_syntax(buffer, cursor),
        std::format(L"{}  {}%",
            status_location(buffer, cursor, mark),
            document_percent(buffer, cursor)
        )
    };
}

int write_clipped_text(int row, int col, std::wstring_view text, int max_width)
{
    if (max_width <= 0) {
        return 0;
    }

    move(row, col);

    int width = 0;
    for (wchar_t wc : text) {
        const int char_width = wchar_width(wc);
        if (width + char_width > max_width) {
            break;
        }
        addnwstr(&wc, 1);
        width += char_width;
    }
    return width;
}

std::wstring display_suffix(std::wstring_view text, int max_width)
{
    std::wstring suffix;
    if (max_width <= 0) {
        return suffix;
    }

    int width = 0;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        const int char_width = wchar_width(*it);
        if (width + char_width > max_width) {
            break;
        }
        suffix.push_back(*it);
        width += char_width;
    }

    std::ranges::reverse(suffix);
    return suffix;
}

void clear_status_row(int row, int cols)
{ mvhline(row, 0, ' ', cols); }

int render_status_text(std::wstring_view text, int row, int cols)
{
    clear_status_row(row, cols);
    return write_clipped_text(row, 0, text, cols);
}

void render_status_line(const StatusLine &status, int row, int cols)
{
    clear_status_row(row, cols);

    const std::wstring right = display_suffix(status.right, cols);
    const int right_width = display_width(right);
    const int right_col = cols - right_width;

    const int center_width = display_width(status.center);
    const int center_col = center_width >= cols
        ? 0
        : (cols - center_width) / 2;
    const int center_max_width = std::max(0, right_col - center_col - 1);
    const int center_written = write_clipped_text(
        row,
        center_col,
        status.center,
        center_max_width
    );

    const int left_max_width = center_written > 0
        ? std::max(0, center_col - 1)
        : std::max(0, right_col - 1);
    write_clipped_text(row, 0, status.left, left_max_width);
    write_clipped_text(row, right_col, right, right_width);
}

struct Stream
{
    static constexpr int pipe_read_chunks = 1;
    static constexpr int default_read_chunks = 16;

    Decoder decoder;
    int fd = -1;
    int saved_stdin_flags = -1;
    bool using_stdin = false;
    bool pipe_like = false;
    bool done = false;

    static bool fd_is_pipe_like(int fd)
    {
        struct stat st {};
        if (fstat(fd, &st) < 0) {
            return true;
        }
        return S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
    }

    int read_chunk_budget() const noexcept
    { return pipe_like ? pipe_read_chunks : default_read_chunks; }

    Stream() = default;

    static Stream from_stdin()
    {
        if (isatty(STDIN_FILENO)) {
            throw UsageError("provide input on stdin or pass file names");
        }

        Stream stream;
        stream.fd = STDIN_FILENO;
        stream.using_stdin = true;
        stream.pipe_like = fd_is_pipe_like(stream.fd);
        stream.saved_stdin_flags = set_nonblocking(stream.fd);
        return stream;
    }

    static Stream from_fifo(const std::filesystem::path &path)
    {
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            throw std::filesystem::filesystem_error{
                "open", path, std::error_code{errno, std::generic_category()}
            };
        }

        Stream stream;
        stream.fd = fd;
        stream.pipe_like = true;
        return stream;
    }

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    Stream(Stream &&other) noexcept
    : decoder{std::move(other.decoder)}
    , fd{std::exchange(other.fd, -1)}
    , saved_stdin_flags{std::exchange(other.saved_stdin_flags, -1)}
    , using_stdin{std::exchange(other.using_stdin, false)}
    , pipe_like{std::exchange(other.pipe_like, false)}
    , done{other.done}
    {}

    Stream &operator=(Stream &&other) noexcept
    {
        if (this != &other) {
            close_current();
            decoder = std::move(other.decoder);
            fd = std::exchange(other.fd, -1);
            saved_stdin_flags = std::exchange(other.saved_stdin_flags, -1);
            using_stdin = std::exchange(other.using_stdin, false);
            pipe_like = std::exchange(other.pipe_like, false);
            done = other.done;
        }
        return *this;
    }

    ~Stream()
    { close_current(); }

    void close_current()
    {
        if (using_stdin && saved_stdin_flags >= 0) {
            (void)fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags);
            saved_stdin_flags = -1;
        } else if (fd >= 0) {
            (void)close(fd);
        }
        fd = -1;
        pipe_like = false;
    }

    bool read_available(TextBuffer &);
};

struct Document
{
    TextBuffer text;
    Syntax::Point cursor = {};
    Syntax::Point top = {};
    std::optional<Syntax::Point> mark;
    std::string source;
    std::optional<Syntax::Language> configured_language;
    std::optional<Stream> stream;
    std::filesystem::path path;
    bool language_detection_pending = true;

    [[nodiscard]] bool input_done() const noexcept
    { return !stream || stream->done; }
};

std::vector<Document> open_documents(const ParsedArguments &arguments)
{
    std::vector<Document> documents;
    documents.reserve(std::max<size_t>(1, arguments.inputs.size()));

    if (arguments.inputs.empty()) {
        Document document;
        document.source = "STDIN";
        document.configured_language = arguments.stdin_language;
        document.stream.emplace(Stream::from_stdin());
        documents.push_back(std::move(document));
        return documents;
    }

    for (const auto &[path, language]: arguments.inputs) {
        std::error_code error;
        const auto status = std::filesystem::status(path, error);
        if (error) {
            throw std::filesystem::filesystem_error{"status", path, error};
        }

        Document document;
        document.source = path.filename().string();
        document.configured_language = language;
        document.path = path;

        if (std::filesystem::is_fifo(status)) {
            document.stream.emplace(Stream::from_fifo(path));
        } else if (std::filesystem::is_regular_file(status)) {
            document.text = TextBuffer{path};
        } else {
            throw UsageError("unsupported input type: " + path.string());
        }

        documents.push_back(std::move(document));
    }

    return documents;
}

inline void Decoder::emit_byte(TextBuffer &doc, unsigned char byte)
{
    const char c = static_cast<char>(byte);
    wchar_t wc = 0;
    const size_t result = std::mbrtowc(&wc, &c, 1, &mb);

    if (result == static_cast<size_t>(-2)) {
        return;
    }

    if (result == static_cast<size_t>(-1)) {
        mb = std::mbstate_t{};
        append_stream_char(doc, replacement_character);
        return;
    }

    if (result == 0) {
        mb = std::mbstate_t{};
        return;
    }

    append_stream_char(doc, wc);
}

inline void Decoder::process_byte(TextBuffer &doc, unsigned char byte)
{
    switch (ansi) {
    case AnsiState::esc:
        if (byte == '[') {
            ansi = AnsiState::csi;
        } else if (byte == ']') {
            ansi = AnsiState::osc;
        } else {
            ansi = AnsiState::normal;
        }
        return;
    case AnsiState::csi:
        if (byte >= 0x40 && byte <= 0x7e) {
            ansi = AnsiState::normal;
        }
        return;
    case AnsiState::osc:
        if (byte == 0x07) {
            ansi = AnsiState::normal;
        } else if (byte == 0x1b) {
            ansi = AnsiState::osc_esc;
        }
        return;
    case AnsiState::osc_esc:
        ansi = byte == '\\' ? AnsiState::normal : AnsiState::osc;
        return;
    case AnsiState::normal:
        break;
    }

    if (byte == 0x1b) {
        if (!std::mbsinit(&mb)) {
            mb = std::mbstate_t{};
            append_stream_char(doc, replacement_character);
        }
        ansi = AnsiState::esc;
        return;
    }

    emit_byte(doc, byte);
}

void Decoder::flush(TextBuffer &doc)
{
    if (!std::mbsinit(&mb)) {
        mb = std::mbstate_t{};
        append_stream_char(doc, replacement_character);
    }
    ansi = AnsiState::normal;
}

inline bool Stream::read_available(TextBuffer &doc)
{
    bool changed = false;
    std::array<unsigned char, 4096> buffer{};

    for (int chunks = 0; chunks < read_chunk_budget() && !done; chunks++) {
        const ssize_t n = read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            auto reparse = doc.suspend_reparse();
            for (size_t i = 0; i < size_t(n); i++) {
                decoder.process_byte(doc, buffer[i]);
            }
            changed = true;
            continue;
        }

        if (n == 0) {
            decoder.flush(doc);
            close_current();
            done = true;
            changed = true;
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            chunks--;
            continue;
        }

        doc.append_ascii("caret: read: ");
        doc.append_ascii(std::strerror(errno));
        doc.append_ascii("\n");
        close_current();
        done = true;
        changed = true;
        break;
    }

    return changed;
}

class Pager
{
    ParsedArguments arguments;
    std::vector<Document> documents;
    size_t document_index = 0;
    std::optional<std::string> copied_text;
    Prompt prompt;
    SearchState search;
    CommandState command;
    std::optional<std::wstring> message;
    InputMode mode = InputMode::normal;
    Keymap<Pager> find_char_forward_keymap;
    Keymap<Pager> find_char_backward_keymap;
    Keymap<Pager> normal_keymap;
    Keymap<Pager> prompt_keymap;
    Keymap<Pager> search_prompt_keymap;
    Keymap<Pager> command_prompt_keymap;
    Keymap<Pager> isearch_keymap;
    KeyDispatcher<Pager> key_dispatcher;
    ScreenMetrics screen;
    bool quit = false;
    bool need_render = true;

    [[nodiscard]] Document &doc()
    { return documents[document_index]; }

    [[nodiscard]] const Document &doc() const
    { return documents[document_index]; }

    void set_mode(InputMode next_mode);
    bool dispatch_key(Key key);

    static void configure_terminal();
    void detect_languages();
    bool read_streams();
    void switch_file(Direction direction);
    void ensure_cursor_visible();
    void align_viewport(ViewportAlignment alignment);

    void move_down();
    void move_up();
    void move_page_down();
    void move_page_up();
    void move_right();
    void move_left();
    void move_next_word();
    void move_previous_word();
    void move_next_matching_bracket();
    void move_previous_matching_bracket();
    void move_matching_bracket();
    void move_line_start();
    void back_to_indentation();
    void move_line_end();

    bool apply_search(const std::wregex &regex, Direction direction);
    bool apply_search(const std::wstring &pattern, Direction direction);
    bool apply_isearch(bool include_origin);
    bool repeat_isearch(Direction direction);
    void repeat_search(bool reverse_direction);
    void search_word(Direction direction);
    void apply_char_find(wchar_t target, Direction direction);
    void begin_prompt(wchar_t leader, InputMode prompt_mode);
    void begin_command_prompt();
    void begin_search_prompt(Direction direction);
    void close_prompt();
    void submit_command_prompt();
    void submit_search_prompt();
    void delete_prompt_character();
    void delete_prompt_character_at_cursor();
    void clear_prompt_to_beginning();
    void clear_prompt_to_end();
    void stop_prompt_history_navigation();
    void move_prompt_left();
    void move_prompt_right();
    void move_prompt_start();
    void move_prompt_end();
    void begin_isearch(Direction direction);
    void isearch_backspace();
    void finish_isearch();
    void cancel_isearch();
    void select_syntax_range(std::optional<Syntax::PointRange> range);
    void select_syntax_at_cursor();
    void expand_syntax_selection();
    void shrink_syntax_selection();
    void next_syntax_selection();
    void previous_syntax_selection();
    void set_mark();
    void exchange_cursor_and_mark();
    void clear_mark();
    std::optional<Syntax::PointRange> active_region() const;
    void copy_range(Syntax::PointRange range);
    void copy_region();
    void copy_to_line_end();

    bool handle_prompt_default(Key key);
    bool handle_isearch_default(Key key);
    bool handle_find_char_key(Direction direction, Key key);
    void render();

public:
    explicit Pager(ParsedArguments parsed_arguments);

    void wait_for_initial_stdin();
    void run();

    [[nodiscard]] const std::optional<std::string> &clipboard() const
    { return copied_text; }
};

void Pager::ensure_cursor_visible()
{
    DisplayRow top = doc().text.display_row_at(doc().top, screen.cols);
    const DisplayRow cursor = doc().text.display_row_at(doc().cursor, screen.cols);

    if (cursor < top) {
        top = cursor;
    } else {
        DisplayRow row = top;
        bool visible = false;
        for (int y = 0; y < screen.content_rows; y++) {
            if (row == cursor) {
                visible = true;
                break;
            }
            const auto next = doc().text.next_display_row(row);
            if (!next) break;
            row = *next;
        }

        if (!visible) {
            top = cursor;
            for (int y = 1; y < screen.content_rows; y++) {
                const auto prev = doc().text.previous_display_row(top);
                if (!prev) break;
                top = *prev;
            }
        }
    }

    doc().top = top.begin();
}

void Pager::align_viewport(ViewportAlignment alignment)
{
    DisplayRow top = doc().text.display_row_at(doc().cursor, screen.cols);
    int rows_before_cursor = 0;

    switch (alignment) {
    case ViewportAlignment::top:
        break;
    case ViewportAlignment::middle:
        rows_before_cursor = screen.content_rows / 2;
        break;
    case ViewportAlignment::bottom:
        rows_before_cursor = screen.content_rows - 1;
        break;
    }

    for (int y = 0; y < rows_before_cursor; y++) {
        const auto prev = doc().text.previous_display_row(top);
        if (!prev) break;
        top = *prev;
    }

    doc().top = top.begin();
}

bool Pager::apply_search(const std::wregex &regex, Direction direction)
{
    if (auto match = doc().text.find_regex(regex, direction, doc().cursor)) {
        doc().mark = match->end;
        doc().cursor = match->start;
        message.reset();
        return true;
    }

    message = L"Not found";
    return false;
}

bool Pager::apply_search(const std::wstring &pattern, Direction direction)
{
    if (pattern.empty()) return false;

    try {
        const std::wregex regex(pattern);
        const bool found = apply_search(regex, direction);
        search.last_regex = regex;
        search.last_direction = direction;
        return found;
    } catch (const std::regex_error &) {
        message = L"Invalid regex";
        return false;
    }
}

bool Pager::apply_isearch(bool include_origin)
{
    IncrementalSearch &incremental = search.incremental;
    if (incremental.pattern.empty()) {
        incremental.match.reset();
        incremental.failed = false;
        return false;
    }

    if (auto match = doc().text.find_literal(
            incremental.pattern,
            incremental.direction,
            incremental.search_origin,
            include_origin
        )) {
        incremental.match = match;
        incremental.failed = false;
        doc().cursor = match->start;
        doc().mark = match->end;
        return true;
    }

    incremental.match.reset();
    incremental.failed = true;
    return false;
}

bool Pager::repeat_isearch(Direction direction)
{
    IncrementalSearch &incremental = search.incremental;
    incremental.direction = direction;
    incremental.search_origin = doc().cursor;
    if (incremental.pattern.empty()) {
        incremental.failed = false;
        incremental.match.reset();
        return true;
    }

    (void)apply_isearch(false);
    return true;
}

void Pager::repeat_search(bool reverse_direction)
{
    if (!search.last_regex) return;

    Direction direction = search.last_direction;
    if (reverse_direction) {
        direction = opposite(direction);
    }
    (void)apply_search(*search.last_regex, direction);
    search.last_direction = direction;
}

void Pager::search_word(Direction direction)
{
    const auto range = doc().text.word_range_at(doc().cursor, true);
    if (!range) return;

    const std::wstring word = doc().text.extract_wide(*range);
    const std::wstring pattern = L"\\b" + escape_regex(word) + L"\\b";

    doc().cursor = range->start;
    search.history.push_back(pattern);
    (void)apply_search(pattern, direction);
}

void Pager::set_mark()
{ doc().mark = doc().cursor; }

void Pager::exchange_cursor_and_mark()
{ if (doc().mark) std::swap(doc().cursor, *doc().mark); }

void Pager::clear_mark()
{ doc().mark.reset(); }

std::optional<Syntax::PointRange> Pager::active_region() const
{
    return doc().mark.transform([&](Syntax::Point anchor) {
        return ordered_range(anchor, doc().cursor);
    });
}

void Pager::copy_region()
{
    const auto region = active_region();
    if (!region) return;

    copy_range(*region);
}

void Pager::copy_range(Syntax::PointRange range)
{
    const auto text = doc().text.extract_multibyte(range);
    if (!text) return;

    copied_text = std::move(*text);
    load_tmux_buffer(*copied_text);
    load_brltty_clipboard(*copied_text);
}

void Pager::copy_to_line_end()
{
    copy_range({doc().cursor, doc().text.line_end(doc().cursor.row)});
}

void Pager::move_down()
{
    const int cols = screen.cols;
    const DisplayRow row = doc().text.display_row_at(doc().cursor, cols);
    if (const auto next = doc().text.next_display_row(row)) {
        const int cursor_x = cursor_display_col(doc().text, row, doc().cursor, cols);
        doc().cursor = point_for_display_col(doc().text, *next, cursor_x, cols);
    }
}

void Pager::move_up()
{
    const int cols = screen.cols;
    const DisplayRow row = doc().text.display_row_at(doc().cursor, cols);
    if (const auto prev = doc().text.previous_display_row(row)) {
        const int cursor_x = cursor_display_col(doc().text, row, doc().cursor, cols);
        doc().cursor = point_for_display_col(doc().text, *prev, cursor_x, cols);
    }
}

void Pager::move_page_down()
{
    const int content_rows = screen.content_rows;
    const int cols = screen.cols;
    const DisplayRow row = doc().text.display_row_at(doc().cursor, cols);
    const auto first_next = doc().text.next_display_row(row);
    if (!first_next) return;

    const int cursor_x = cursor_display_col(doc().text, row, doc().cursor, cols);
    DisplayRow target = *first_next;
    for (int i = 1; i < content_rows; i++) {
        const auto next = doc().text.next_display_row(target);
        if (!next) break;
        target = *next;
    }
    doc().cursor = point_for_display_col(doc().text, target, cursor_x, cols);
}

void Pager::move_page_up()
{
    const int content_rows = screen.content_rows;
    const int cols = screen.cols;
    const DisplayRow row = doc().text.display_row_at(doc().cursor, cols);
    const int cursor_x = cursor_display_col(doc().text, row, doc().cursor, cols);
    DisplayRow target = row;
    for (int i = 0; i < content_rows; i++) {
        const auto prev = doc().text.previous_display_row(target);
        if (!prev) break;
        target = *prev;
    }
    doc().cursor = point_for_display_col(doc().text, target, cursor_x, cols);
}

void Pager::move_right()
{
    const int cols = screen.cols;
    const DisplayRow row = doc().text.display_row_at(doc().cursor, cols);
    const int cursor_x = cursor_display_col(doc().text, row, doc().cursor, cols);
    const int max_col = clamp_col_for_row(row.width, row.width, cols);
    if (cursor_x < max_col) {
        doc().cursor = point_for_display_col(doc().text, row, cursor_x + 1, cols);
    } else if (const auto next = doc().text.next_display_row(row)) {
        doc().cursor = point_for_display_col(doc().text, *next, 0, cols);
    }
}

void Pager::move_left()
{
    const int cols = screen.cols;
    const DisplayRow row = doc().text.display_row_at(doc().cursor, cols);
    const int cursor_x = cursor_display_col(doc().text, row, doc().cursor, cols);
    if (cursor_x > 0) {
        doc().cursor = point_for_display_col(doc().text, row, cursor_x - 1, cols);
    } else if (const auto prev = doc().text.previous_display_row(row)) {
        const int previous_col = clamp_col_for_row(prev->width, prev->width, cols);
        doc().cursor = point_for_display_col(doc().text, *prev, previous_col, cols);
    }
}

enum class WordKind { whitespace, separator, other };

bool is_word_separator(wchar_t wc)
{ return word_separators.contains(wc); }

std::wstring escape_regex(std::wstring_view literal)
{
    constexpr std::wstring_view metacharacters = LR"(\.^$|()[]{}*+?)";
    std::wstring escaped;
    escaped.reserve(literal.size());

    for (const wchar_t wc: literal) {
        if (metacharacters.contains(wc)) escaped.push_back(L'\\');
        escaped.push_back(wc);
    }

    return escaped;
}

WordKind word_kind(wchar_t wc)
{
    if (std::iswspace(static_cast<wint_t>(wc))) {
        return WordKind::whitespace;
    }
    if (is_word_separator(wc)) {
        return WordKind::separator;
    }
    return WordKind::other;
}

std::optional<WordKind> word_kind_at(const TextBuffer &doc, Syntax::Point position)
{
    const auto wc = doc.char_at(position);
    if (!wc) return std::nullopt;
    return word_kind(*wc);
}

constexpr std::wstring_view brackets_for(Direction direction)
{
    return bracket_chars[static_cast<size_t>(direction)];
}

std::optional<wchar_t> matching_bracket(wchar_t wc, Direction direction)
{
    const size_t index = brackets_for(direction).find(wc);
    if (index != std::wstring_view::npos) {
        return brackets_for(opposite(direction))[index];
    }
    return std::nullopt;
}

bool is_bracket(wchar_t wc, Direction direction)
{
    return brackets_for(direction).contains(wc);
}

std::optional<Direction> bracket_direction(wchar_t wc)
{
    for (auto dir: {Direction::Forward, Direction::Backward})
        if (is_bracket(wc, dir)) return dir;

    return std::nullopt;
}

std::optional<Syntax::Point> bracket_near_cursor(
    const TextBuffer &doc,
    Syntax::Point cursor,
    Direction direction
)
{
    const auto is_direction_bracket_at = [&](Syntax::Point position) {
        const auto wc = doc.char_at(position);
        return wc && is_bracket(*wc, direction);
    };

    if (direction == Direction::Forward) {
        if (is_direction_bracket_at(cursor)) {
            return cursor;
        }
        if (const auto next = doc.next_position(cursor);
            next && is_direction_bracket_at(*next)) {
            return next;
        }
        return std::nullopt;
    }

    if (const auto previous = doc.previous_position(cursor);
        previous && is_direction_bracket_at(*previous)) {
        return previous;
    }
    if (is_direction_bracket_at(cursor)) {
        return cursor;
    }
    return std::nullopt;
}

bool advance_bracket_scan(
    const TextBuffer &doc,
    Syntax::Point &position,
    Direction direction
)
{
    if (direction == Direction::Forward) {
        if (const auto next = doc.next_position(position)) {
            position = *next;
            return true;
        }
        return false;
    }

    if (const auto previous = doc.previous_position(position)) {
        position = *previous;
        return true;
    }
    return false;
}

std::optional<Syntax::Point> matching_bracket_position(
    const TextBuffer &doc,
    Syntax::Point start,
    Direction start_direction
)
{
    const auto start_bracket = doc.char_at(start);
    assert(start_bracket.has_value());
    const auto match_bracket = matching_bracket(*start_bracket, start_direction);
    assert(match_bracket.has_value());

    size_t depth = 1;
    Syntax::Point position = start;
    while (advance_bracket_scan(doc, position, start_direction)) {
        const auto wc = doc.char_at(position);
        assert(wc.has_value());
        if (*wc == *start_bracket) {
            depth++;
        } else if (*wc == *match_bracket) {
            if (depth == 1) {
                return position;
            }
            depth--;
        }
    }

    return std::nullopt;
}

void Pager::move_next_word()
{
    Syntax::Point position = doc().cursor;
    const Syntax::Point end = doc().text.end();

    if (const auto current_kind = word_kind_at(doc().text, position);
        current_kind && *current_kind != WordKind::whitespace) {
        while (position < end && word_kind_at(doc().text, position) == current_kind) {
            const auto next = doc().text.next_position(position);
            if (!next) break;
            position = *next;
        }
    }

    while (position < end) {
        const auto kind = word_kind_at(doc().text, position);
        assert(kind.has_value());
        if (*kind != WordKind::whitespace) {
            doc().cursor = position;
            return;
        }
        const auto next = doc().text.next_position(position);
        if (!next) break;
        position = *next;
    }
    doc().cursor = end;
}

void Pager::move_previous_word()
{
    std::optional<Syntax::Point> position = doc().text.previous_position(doc().cursor);
    while (position) {
        const auto kind = word_kind_at(doc().text, *position);
        assert(kind.has_value());
        if (kind != WordKind::whitespace) {
            Syntax::Point begin = *position;
            while (const auto previous = doc().text.previous_position(begin)) {
                if (word_kind_at(doc().text, *previous) != kind) {
                    break;
                }
                begin = *previous;
            }
            doc().cursor = begin;
            return;
        }
        position = doc().text.previous_position(*position);
    }
    doc().cursor = {0, 0};
}

void Pager::move_next_matching_bracket()
{
    const auto opening = bracket_near_cursor(doc().text, doc().cursor, Direction::Forward);
    if (!opening) {
        move_next_word();
        return;
    }

    if (const auto match = matching_bracket_position(doc().text, *opening, Direction::Forward)) {
        doc().cursor = *match;
    }
}

void Pager::move_previous_matching_bracket()
{
    const auto closing = bracket_near_cursor(doc().text, doc().cursor, Direction::Backward);
    if (!closing) {
        move_previous_word();
        return;
    }

    if (const auto match = matching_bracket_position(doc().text, *closing, Direction::Backward)) {
        doc().cursor = *match;
    }
}

void Pager::move_matching_bracket()
{
    auto bracket = doc().cursor;
    auto direction = doc().text.char_at(bracket).and_then(bracket_direction);

    while (!direction) {
        const auto next = doc().text.next_position(bracket);
        if (!next || next->row != doc().cursor.row) return;

        bracket = *next;
        const auto wc = doc().text.char_at(bracket);
        if (!wc) return;
        direction = bracket_direction(*wc);
    }

    if (const auto match = matching_bracket_position(doc().text, bracket, *direction)) {
        doc().cursor = *match;
    }
}

void Pager::move_line_start()
{
    doc().cursor = {doc().cursor.row, 0};
}

void Pager::back_to_indentation()
{
    const uint32_t row = doc().cursor.row;
    const Syntax::Point end = doc().text.line_end(row);
    Syntax::Point position{row, 0};

    while (position.byte_column < end.byte_column) {
        const auto wc = doc().text.char_at(position);
        if (!wc || !std::iswspace(static_cast<wint_t>(*wc))) break;

        const auto next = doc().text.next_position(position);
        if (!next || next->row != row) {
            position = end;
            break;
        }
        position = *next;
    }

    doc().cursor = position;
}

void Pager::move_line_end()
{
    doc().cursor = doc().text.line_end(doc().cursor.row);
}

void Pager::apply_char_find(wchar_t target, Direction direction)
{
    const std::wstring pattern(1, target);
    if (const auto match = doc().text.find_literal(
            pattern,
            direction,
            doc().cursor,
            false,
            true
        )) {
        doc().cursor = match->start;
    }
}

bool Pager::handle_find_char_key(Direction direction, Key key)
{
    if (auto ch = printable(key)) {
        apply_char_find(*ch, direction);
    }

    return true;
}

void Pager::begin_prompt(wchar_t leader, InputMode prompt_mode)
{
    prompt.active = true;
    prompt.leader = leader;
    prompt.text.clear();
    prompt.cursor = 0;
    message.reset();
    set_mode(prompt_mode);
}

void Pager::begin_command_prompt()
{
    command.prompt.reset();
    begin_prompt(L':', InputMode::command_prompt);
}

void Pager::begin_search_prompt(Direction direction)
{
    search.prompt.direction = direction;
    search.prompt.history.reset();
    begin_prompt(
        direction == Direction::Forward ? L'/' : L'?',
        InputMode::search_prompt
    );
}

void Pager::close_prompt()
{
    prompt.active = false;
    prompt.text.clear();
    prompt.cursor = 0;
    search.prompt.history.reset();
    command.prompt.reset();
    set_mode(InputMode::normal);
}

void Pager::switch_file(Direction direction)
{
    if ((direction == Direction::Forward
            && document_index + 1 >= documents.size())
        || (direction == Direction::Backward && document_index == 0)) {
        message = direction == Direction::Forward
            ? L"No next file"
            : L"No previous file";
        return;
    }

    if (direction == Direction::Forward) {
        document_index++;
    } else {
        document_index--;
    }
    message.reset();
    need_render = true;
}

void Pager::submit_command_prompt()
{
    const std::wstring submitted_command = prompt.text;
    close_prompt();
    if (submitted_command.empty()) return;
    command.history.push_back(submitted_command);

    if (submitted_command == L"n") {
        switch_file(Direction::Forward);
        return;
    }
    if (submitted_command == L"p") {
        switch_file(Direction::Backward);
        return;
    }

    const bool numeric = std::ranges::all_of(submitted_command, [](wchar_t ch) {
        return ch >= L'0' && ch <= L'9';
    });
    if (!numeric) {
        message = L"Unknown command";
        return;
    }

    const size_t line_count = doc().text.line_count();
    size_t line = 0;
    for (wchar_t ch : submitted_command) {
        const auto digit = static_cast<size_t>(ch - L'0');
        if (line >= line_count
            || digit > line_count
            || line > (line_count - digit) / 10) {
            line = line_count;
            break;
        }
        line = line * 10 + digit;
    }

    const size_t row = line == 0
        ? 0
        : std::min(line - 1, line_count - 1);
    doc().cursor = {static_cast<uint32_t>(row), 0};
}

void Pager::submit_search_prompt()
{
    const Direction direction = search.prompt.direction;
    const std::wstring pattern = prompt.text;
    close_prompt();
    if (!pattern.empty()) {
        search.history.push_back(pattern);
    }

    (void)apply_search(pattern, direction);
}

void Pager::delete_prompt_character()
{
    if (prompt.text.empty()) {
        close_prompt();
        return;
    }
    if (prompt.cursor == 0) return;

    stop_prompt_history_navigation();
    prompt.text.erase(prompt.cursor - 1, 1);
    prompt.cursor--;
}

void Pager::delete_prompt_character_at_cursor()
{
    if (prompt.cursor == prompt.text.size()) return;

    stop_prompt_history_navigation();
    prompt.text.erase(prompt.cursor, 1);
}

void Pager::clear_prompt_to_beginning()
{
    if (prompt.cursor == 0) return;

    stop_prompt_history_navigation();
    prompt.text.erase(0, prompt.cursor);
    prompt.cursor = 0;
}

void Pager::clear_prompt_to_end()
{
    if (prompt.cursor == prompt.text.size()) return;

    stop_prompt_history_navigation();
    prompt.text.erase(prompt.cursor);
}

void Pager::move_prompt_left()
{ if (prompt.cursor > 0) prompt.cursor--; }

void Pager::move_prompt_right()
{ if (prompt.cursor < prompt.text.size()) prompt.cursor++; }

void Pager::move_prompt_start()
{ prompt.cursor = 0; }

void Pager::move_prompt_end()
{ prompt.cursor = prompt.text.size(); }

void Pager::stop_prompt_history_navigation()
{
    if (mode == InputMode::search_prompt) {
        search.prompt.history.stop_navigation();
    } else if (mode == InputMode::command_prompt) {
        command.prompt.stop_navigation();
    }
}

void Pager::begin_isearch(Direction direction)
{
    IncrementalSearch &incremental = search.incremental;
    incremental.active = true;
    incremental.direction = direction;
    incremental.pattern.clear();
    incremental.restore_cursor = doc().cursor;
    incremental.restore_mark = doc().mark;
    incremental.search_origin = doc().cursor;
    incremental.match.reset();
    incremental.failed = false;
    message.reset();
    set_mode(InputMode::isearch);
}

void Pager::isearch_backspace()
{
    IncrementalSearch &incremental = search.incremental;
    if (incremental.pattern.empty()) {
        finish_isearch();
        return;
    }
    incremental.pattern.pop_back();
    (void)apply_isearch(true);
};


void Pager::finish_isearch()
{
    IncrementalSearch &incremental = search.incremental;
    incremental.active = false;
    incremental.pattern.clear();
    incremental.match.reset();
    incremental.failed = false;
    set_mode(InputMode::normal);
}

void Pager::cancel_isearch()
{
    IncrementalSearch &incremental = search.incremental;
    doc().cursor = incremental.restore_cursor;
    doc().mark = incremental.restore_mark;
    finish_isearch();
}

void Pager::select_syntax_range(std::optional<Syntax::PointRange> range)
{
    if (!range) {
        return;
    }

    doc().cursor = range->start;
    doc().mark = range->end;
}

void Pager::select_syntax_at_cursor()
{
    select_syntax_range(doc().text.syntax_range_at(doc().cursor));
}

void Pager::expand_syntax_selection()
{
    if (const auto range = active_region()) {
        select_syntax_range(doc().text.syntax_expand_range(*range));
    }
}

void Pager::shrink_syntax_selection()
{
    if (const auto range = active_region()) {
        select_syntax_range(doc().text.syntax_shrink_range(*range));
    }
}

void Pager::next_syntax_selection()
{
    if (const auto range = active_region()) {
        select_syntax_range(doc().text.syntax_next_range(*range));
    }
}

void Pager::previous_syntax_selection()
{
    if (const auto range = active_region()) {
        select_syntax_range(doc().text.syntax_previous_range(*range));
    }
}

struct PromptDisplay
{
    std::wstring text;
    int cursor_col = 0;
};

PromptDisplay prompt_display(const Prompt &prompt, int cols)
{
    if (cols <= 0) return {};

    const int cursor_width = std::max(0, cols - 2);
    size_t begin = prompt.cursor;
    int width_before_cursor = 0;
    while (begin > 0) {
        const int char_width = wchar_width(prompt.text[begin - 1]);
        if (width_before_cursor + char_width > cursor_width) break;
        begin--;
        width_before_cursor += char_width;
    }

    std::wstring text(1, prompt.leader);
    int visible_width = 0;
    const int available_width = cols - 1;
    for (size_t i = begin; i < prompt.text.size(); i++) {
        const int char_width = wchar_width(prompt.text[i]);
        if (visible_width + char_width > available_width) break;
        text.push_back(prompt.text[i]);
        visible_width += char_width;
    }

    return {
        .text = std::move(text),
        .cursor_col = std::min(cols - 1, 1 + width_before_cursor),
    };
}

std::wstring isearch_text(const IncrementalSearch &incremental)
{
    std::wstring text = incremental.failed
        ? L"Failing incremental search "
        : L"Incremental search ";
    text += incremental.direction == Direction::Forward ? L"forward: " : L"backward: ";
    text += incremental.pattern;
    return text;
}

void recall_previous(
    const std::vector<std::wstring> &entries,
    PromptHistory &history,
    Prompt &input
)
{
    if (entries.empty()) {
        return;
    }

    if (!history.index) {
        history.draft = input.text;
        history.draft_cursor = input.cursor;
        history.index = entries.size() - 1;
    } else if (*history.index > 0) {
        (*history.index)--;
    }
    input.text = entries[*history.index];
    input.cursor = input.text.size();
}

void recall_next(
    const std::vector<std::wstring> &entries,
    PromptHistory &history,
    Prompt &input
)
{
    if (!history.index) return;

    if (*history.index + 1 < entries.size()) {
        (*history.index)++;
        input.text = entries[*history.index];
        input.cursor = input.text.size();
        return;
    }

    input.text = history.draft;
    input.cursor = history.draft_cursor;
    history.stop_navigation();
}

Pager::Pager(ParsedArguments parsed_arguments)
: arguments{std::move(parsed_arguments)}
, documents{open_documents(arguments)}
, find_char_forward_keymap{
    [](Pager &pager, Key key) {
        return pager.handle_find_char_key(Direction::Forward, key);
    }
}
, find_char_backward_keymap{
    [](Pager &pager, Key key) {
        return pager.handle_find_char_key(Direction::Backward, key);
    }
}
, prompt_keymap{&Pager::handle_prompt_default}
, isearch_keymap{&Pager::handle_isearch_default}
, key_dispatcher{normal_keymap}
{
    normal_keymap.set(kbd("<resize>"), [](Pager&) {});
    prompt_keymap.set(kbd("<resize>"), [](Pager&) {});
    isearch_keymap.set(kbd("<resize>"), [](Pager&) {});

    normal_keymap.set(kbd("<down>"),    &Pager::move_down);
    normal_keymap.set(kbd("<up>"),      &Pager::move_up);
    normal_keymap.set(kbd("<pgdown>"),  &Pager::move_page_down);
    normal_keymap.set(kbd("<pgup>"),    &Pager::move_page_up);
    normal_keymap.set(kbd("<right>"),   &Pager::move_right);
    normal_keymap.set(kbd("<left>"),    &Pager::move_left);
    normal_keymap.set(kbd("M-b"),       &Pager::move_previous_word);
    normal_keymap.set(kbd("M-f"),       &Pager::move_next_word);
    normal_keymap.set(kbd("C-M-b"),     &Pager::move_previous_matching_bracket);
    normal_keymap.set(kbd("C-M-f"),     &Pager::move_next_matching_bracket);
    normal_keymap.set(kbd("q"),         [](Pager &pager) { pager.quit = true; });
    normal_keymap.set(kbd("Q"),         [](Pager &pager) { pager.quit = true; });
    normal_keymap.set(kbd("C-c"),       [](Pager &pager) { pager.quit = true; });
    normal_keymap.set(kbd("C-d"),       [](Pager &pager) { pager.quit = true; });
    normal_keymap.set(kbd("C-<space>"), &Pager::set_mark);
    normal_keymap.set(kbd("M-w"),       &Pager::copy_region);
    normal_keymap.set(kbd("C-k"),       &Pager::copy_to_line_end);
    normal_keymap.set(kbd("<esc>"),     &Pager::clear_mark);
    normal_keymap.set(kbd("x"),         &Pager::exchange_cursor_and_mark);
    normal_keymap.set(kbd("u"),         &Pager::previous_syntax_selection);
    normal_keymap.set(kbd("i"),         &Pager::shrink_syntax_selection);
    normal_keymap.set(kbd("o"),         &Pager::expand_syntax_selection);
    normal_keymap.set(kbd("p"),         &Pager::next_syntax_selection);
    normal_keymap.set(kbd(":"),         &Pager::begin_command_prompt);
    normal_keymap.set(kbd("/"),         [](Pager &pager) { pager.begin_search_prompt(Direction::Forward); });
    normal_keymap.set(kbd("?"),         [](Pager &pager) { pager.begin_search_prompt(Direction::Backward); });
    normal_keymap.set(kbd("*"),         [](Pager &pager) { pager.search_word(Direction::Forward); });
    normal_keymap.set(kbd("#"),         [](Pager &pager) { pager.search_word(Direction::Backward); });
    normal_keymap.set(kbd("n"),         [](Pager &pager) { pager.repeat_search(false); });
    normal_keymap.set(kbd("N"),         [](Pager &pager) { pager.repeat_search(true); });
    normal_keymap.set(kbd("%"),         &Pager::move_matching_bracket);
    normal_keymap.set(kbd("C-s"),       [](Pager &pager) { pager.begin_isearch(Direction::Forward); });
    normal_keymap.set(kbd("C-r"),       [](Pager &pager) { pager.begin_isearch(Direction::Backward); });
    normal_keymap.set(kbd("f"),         find_char_forward_keymap);
    normal_keymap.set(kbd("F"),         find_char_backward_keymap);
    normal_keymap.set(kbd("<ret>"),     &Pager::select_syntax_at_cursor);
    normal_keymap.set(kbd("j"),         &Pager::move_down);
    normal_keymap.set(kbd("k"),         &Pager::move_up);
    normal_keymap.set(kbd("C-f"),       &Pager::move_page_down);
    normal_keymap.set(kbd("<space>"),   &Pager::move_page_down);
    normal_keymap.set(kbd("C-b"),       &Pager::move_page_up);
    normal_keymap.set(kbd("C-l"),       [](Pager &pager) {
        pager.align_viewport(ViewportAlignment::middle);
    });
    normal_keymap.set(kbd("z t"),       [](Pager &pager) {
        pager.align_viewport(ViewportAlignment::top);
    });
    normal_keymap.set(kbd("z z"),       [](Pager &pager) {
        pager.align_viewport(ViewportAlignment::middle);
    });
    normal_keymap.set(kbd("z b"),       [](Pager &pager) {
        pager.align_viewport(ViewportAlignment::bottom);
    });
    normal_keymap.set(kbd("l"),         &Pager::move_right);
    normal_keymap.set(kbd("h"),         &Pager::move_left);
    normal_keymap.set(kbd("\x7f"),      &Pager::move_left);
    normal_keymap.set(kbd("C-a"),       &Pager::move_line_start);
    normal_keymap.set(kbd("0"),         &Pager::move_line_start);
    normal_keymap.set(kbd("^"),         &Pager::back_to_indentation);
    normal_keymap.set(kbd("M-m"),       &Pager::back_to_indentation);
    normal_keymap.set(kbd("C-e"),       &Pager::move_line_end);
    normal_keymap.set(kbd("$"),         &Pager::move_line_end);
    normal_keymap.set(kbd("<home>"),    &Pager::move_line_start);
    normal_keymap.set(kbd("<end>"),     &Pager::move_line_end);
    normal_keymap.set(kbd("g g"),       [](Pager &pager) { pager.doc().cursor = {0, 0}; });
    normal_keymap.set(kbd("G"),         [](Pager &pager) { pager.doc().cursor = pager.doc().text.end(); });
    normal_keymap.set(kbd("M-<"),       [](Pager &pager) { pager.doc().cursor = {0, 0}; });
    normal_keymap.set(kbd("M->"),       [](Pager &pager) { pager.doc().cursor = pager.doc().text.end(); });

    prompt_keymap.set(kbd("<esc>"), &Pager::close_prompt);
    prompt_keymap.set(kbd("C-g"), &Pager::close_prompt);
    prompt_keymap.set(kbd("\x7f"), &Pager::delete_prompt_character);
    prompt_keymap.set(kbd("<backspace>"), &Pager::delete_prompt_character);
    prompt_keymap.set(kbd("C-h"), &Pager::delete_prompt_character);
    prompt_keymap.set(kbd("<del>"), &Pager::delete_prompt_character_at_cursor);
    prompt_keymap.set(kbd("C-u"), &Pager::clear_prompt_to_beginning);
    prompt_keymap.set(kbd("C-k"), &Pager::clear_prompt_to_end);
    prompt_keymap.set(kbd("<left>"), &Pager::move_prompt_left);
    prompt_keymap.set(kbd("C-b"), &Pager::move_prompt_left);
    prompt_keymap.set(kbd("<right>"), &Pager::move_prompt_right);
    prompt_keymap.set(kbd("C-f"), &Pager::move_prompt_right);
    prompt_keymap.set(kbd("<home>"), &Pager::move_prompt_start);
    prompt_keymap.set(kbd("C-a"), &Pager::move_prompt_start);
    prompt_keymap.set(kbd("<end>"), &Pager::move_prompt_end);
    prompt_keymap.set(kbd("C-e"), &Pager::move_prompt_end);

    search_prompt_keymap = prompt_keymap;
    search_prompt_keymap.set(kbd("<ret>"), &Pager::submit_search_prompt);
    search_prompt_keymap.set(kbd("<up>"), [](Pager &pager) {
        recall_previous(
            pager.search.history,
            pager.search.prompt.history,
            pager.prompt
        );
    });
    search_prompt_keymap.set(kbd("<down>"), [](Pager &pager) {
        recall_next(
            pager.search.history,
            pager.search.prompt.history,
            pager.prompt
        );
    });

    command_prompt_keymap = prompt_keymap;
    command_prompt_keymap.set(kbd("<ret>"), &Pager::submit_command_prompt);
    command_prompt_keymap.set(kbd("<up>"), [](Pager &pager) {
        recall_previous(pager.command.history, pager.command.prompt, pager.prompt);
    });
    command_prompt_keymap.set(kbd("<down>"), [](Pager &pager) {
        recall_next(pager.command.history, pager.command.prompt, pager.prompt);
    });

    isearch_keymap.set(kbd("C-g"), &Pager::cancel_isearch);
    isearch_keymap.set(kbd("C-s"), [](Pager &pager) {
        (void)pager.repeat_isearch(Direction::Forward);
    });
    isearch_keymap.set(kbd("C-r"), [](Pager &pager) {
        (void)pager.repeat_isearch(Direction::Backward);
    });
    isearch_keymap.set(kbd("\x7f"), &Pager::isearch_backspace);
    isearch_keymap.set(kbd("C-h"), &Pager::isearch_backspace);
    isearch_keymap.set(kbd("<backspace>"), &Pager::isearch_backspace);

    for (Document &document: documents) {
        if (!document.configured_language) continue;
        if (!document.text.set_language(*document.configured_language)) {
            throw UsageError("failed to set language");
        }
        document.language_detection_pending = false;
    }
    detect_languages();
}

void Pager::detect_languages()
{
    for (Document &document: documents) {
        if (!document.language_detection_pending) continue;

        const auto bytes = document.text.bytes();
        if (!document.input_done() && !bytes.contains(u8'\n')) continue;

        document.language_detection_pending = false;
        const bool using_stdin = document.stream && document.stream->using_stdin;
        const auto language = Syntax::language_from_shebang(bytes)
            .or_else([&]{
                return using_stdin
                    ? std::nullopt
                    : Syntax::language_from(document.path);
            })
            .or_else([&]{
                return using_stdin
                    ? std::optional{Syntax::Language::Markdown}
                    : std::nullopt;
            });

        if (language && !document.text.set_language(*language)) {
            throw UsageError("failed to set language");
        }
    }
}

void Pager::set_mode(InputMode next_mode)
{
    if (mode == next_mode) return;

    mode = next_mode;
    switch (mode) {
    case InputMode::normal:
        key_dispatcher.set_active_keymap(normal_keymap);
        break;
    case InputMode::search_prompt:
        key_dispatcher.set_active_keymap(search_prompt_keymap);
        break;
    case InputMode::command_prompt:
        key_dispatcher.set_active_keymap(command_prompt_keymap);
        break;
    case InputMode::isearch:
        key_dispatcher.set_active_keymap(isearch_keymap);
        break;
    }
}

bool Pager::dispatch_key(Key key)
{
    return key_dispatcher.feed(key, *this) != KeyDispatcher<Pager>::Result::rejected;
}

bool Pager::handle_prompt_default(Key key)
{
    if (auto ch = printable(key)) {
        stop_prompt_history_navigation();
        prompt.text.insert(prompt.cursor, 1, *ch);
        prompt.cursor++;
        return true;
    }
    return false;
}

bool Pager::handle_isearch_default(Key key)
{
    if (auto ch = printable(key)) {
        IncrementalSearch &incremental = search.incremental;
        incremental.pattern.push_back(*ch);
        (void)apply_isearch(true);
        return true;
    }

    finish_isearch();
    (void)dispatch_key(key);
    return true;
}

void Pager::render()
{
    DisplayRow row = doc().text.display_row_at(doc().top, screen.cols);
    const DisplayRow cursor_row = doc().text.display_row_at(doc().cursor, screen.cols);
    const auto region = active_region();
    int cursor_y = -1;
    int cursor_x = 0;

    erase();
    for (int y = 0; y < screen.content_rows; y++) {
        const std::wstring line = doc().text.line_wide(row.row);
        const size_t begin = doc().text.wide_column({row.row, row.begin_column});
        const size_t end = doc().text.wide_column({row.row, row.end_column});
        move(y, 0);
        for (size_t i = begin; i < end; i++) {
            const Syntax::Point position{
                row.row,
                doc().text.byte_column_for_wide_index(row.row, i)
            };
            const bool selected = region && region->contains(position);
            if (selected) {
                attron(A_REVERSE);
            }
            addnwstr(line.data() + i, 1);
            if (selected) {
                attroff(A_REVERSE);
            }
        }

        if (row == cursor_row) {
            cursor_y = y;
            cursor_x = cursor_display_col(doc().text, row, doc().cursor, screen.cols);
        }

        const auto next = doc().text.next_display_row(row);
        if (!next) break;
        row = *next;
    }

    std::optional<std::wstring> status_override;
    std::optional<int> status_cursor;
    if (search.incremental.active) {
        status_override = isearch_text(search.incremental);
    } else if (prompt.active) {
        PromptDisplay display = prompt_display(prompt, screen.cols);
        status_override = std::move(display.text);
        status_cursor = display.cursor_col;
    } else if (message) {
        status_override = message;
    }

    if (screen.rows > 1) {
        const int status_row = screen.rows - 1;
        attron(A_REVERSE);

        if (status_override) {
            render_status_text(*status_override, status_row, screen.cols);
        } else {
            render_status_line(
                status_line(doc().text, doc().cursor, doc().mark, doc().source),
                status_row,
                screen.cols
            );
        }

        attroff(A_REVERSE);
        if (status_cursor) {
            move(status_row, *status_cursor);
            ::refresh();
            return;
        }
    }

    assert(cursor_y >= 0);
    move(cursor_y, cursor_x);
    ::refresh();
}

inline void wait_for_activity(const std::vector<Document> &documents, int tty_fd)
{
    std::vector<pollfd> fds;
    fds.reserve(documents.size() + 1);
    for (const Document &document: documents) {
        if (document.stream
            && !document.stream->done && document.stream->fd >= 0) {
            fds.push_back({
                .fd = document.stream->fd,
                .events = POLLIN,
                .revents = 0,
            });
        }
    }

    fds.push_back({.fd = tty_fd, .events = POLLIN, .revents = 0});

    (void)poll(fds.data(), fds.size(), 100);
}

void wait_for_stdin_available(int fd)
{
    pollfd pfd{ .fd = fd, .events = POLLIN, .revents = 0 };

    for (;;) {
        int r = poll(&pfd, 1, -1);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            fatal("poll");
        }

        if (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
            return;
    }
}

void Pager::wait_for_initial_stdin()
{
    const auto &stream = doc().stream;
    if (stream && stream->using_stdin && stream->fd == STDIN_FILENO) {
        wait_for_stdin_available(stream->fd);
    }
}

bool Pager::read_streams()
{
    std::vector<pollfd> fds;
    std::vector<Document *> polled_documents;
    fds.reserve(documents.size());
    polled_documents.reserve(documents.size());

    for (Document &document: documents) {
        if (!document.stream
            || document.stream->done || document.stream->fd < 0) {
            continue;
        }
        fds.push_back({
            .fd = document.stream->fd,
            .events = POLLIN,
            .revents = 0,
        });
        polled_documents.push_back(&document);
    }

    if (fds.empty()) return false;
    const int result = poll(fds.data(), fds.size(), 0);
    if (result < 0) {
        if (errno == EINTR) return false;
        fatal("poll");
    }
    if (result == 0) return false;

    bool activity = false;
    for (auto [fd, document]: std::views::zip(fds, polled_documents)) {
        if (!(fd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            continue;

        if (!document->stream->read_available(document->text)) continue;
        activity = true;
        document->cursor = document->text.clamp(document->cursor);
        document->top = document->text.clamp(document->top);
        document->mark = document->mark.transform(
            [&](Syntax::Point mark) { return document->text.clamp(mark); }
        );

        if (document == &doc() && search.incremental.active) {
            auto &incremental = search.incremental;
            incremental.restore_cursor = document->text.clamp(
                incremental.restore_cursor
            );
            incremental.search_origin = document->text.clamp(
                incremental.search_origin
            );
            incremental.restore_mark = incremental.restore_mark.transform(
                [&](Syntax::Point mark) { return document->text.clamp(mark); }
            );
        }
    }
    return activity;
}

void Pager::configure_terminal()
{
    cbreak();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    intrflush(stdscr, FALSE);
    curs_set(1);
    set_escdelay(25);
}

void Pager::run()
{
    Terminal terminal;
    configure_terminal();
    KeyReader read_key{stdscr};
    terminal.disable_flow_control();
    const int tty_fd = terminal.input_fd();

    while (!quit) {
        const bool input_activity = read_streams();
        detect_languages();

        screen = ScreenMetrics::current();
        ensure_cursor_visible();

        bool handled_key = false;
        while (auto key = read_key()) {
            if (message) {
                message.reset();
                handled_key = true;
            }

            if (dispatch_key(*key)) handled_key = true;
            if (quit) break;
            ensure_cursor_visible();
        }

        if (input_activity || handled_key || need_render) {
            screen = ScreenMetrics::current();
            ensure_cursor_visible();
            render();
            need_render = false;
        }

        if (!quit && !input_activity && !handled_key) {
            wait_for_activity(documents, tty_fd);
        }
    }
}

} // namespace

int main(int argc, char **argv)
try {
    std::setlocale(LC_ALL, "");

    ParsedArguments arguments = parse_arguments(argc, argv);
    if (arguments.list_languages) {
        language_listing(stdout);
        return EXIT_SUCCESS;
    }

    Pager pager(std::move(arguments));
    pager.wait_for_initial_stdin();
    pager.run();

    if (auto clipboard = pager.clipboard()) {
        write_all_or_throw(STDOUT_FILENO, *clipboard);
    }

    return EXIT_SUCCESS;
} catch (const UsageError &error) {
    std::println(stderr, "caret: {}", error.what());
    return 2;
} catch (const std::exception &error) {
    std::println(stderr, "caret: {}", error.what());
    return EXIT_FAILURE;
}
