#pragma once

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <curses.h>

namespace mlang::curses {

struct Char {
    wchar_t ch{};
    bool meta = false;

    friend bool operator==(Char, Char) = default;
};

struct Code {
    int fn;

    friend bool operator==(Code, Code) = default;
};

using Key = std::variant<Char, Code>;

// Assumes either nodelay or timeout < ESCDELAY and keypad mode.
class KeyReader
{
    WINDOW* win;
    std::optional<Code> buffered;

public:
    KeyReader(WINDOW* win = stdscr): win{win} {}

    std::optional<Key> operator()()
    {
        wint_t wch{};

        if (buffered) {
            const auto code = *buffered;
            buffered.reset();
            return code;
        }
        switch (wget_wch(win, &wch)) {
        case OK: {
            auto ch = static_cast<wchar_t>(wch);
            if (ch == L'\x1B') {
                // At this point ncurses has already waited ESCDELAY and decided
                // this is not a known keypad sequence. A following Meta
                // character, if any, is already buffered in curses' FIFO,
                // so this read must be immediate.
                switch (wget_wch(win, &wch)) {
                // ESC prefix for Meta sequence
                case OK:
                    return Char{static_cast<wchar_t>(wch), true};
                case KEY_CODE_YES:
                    buffered = Code{static_cast<int>(wch)};
                    break;
                case ERR:
                default:
                    break;
                }
            }
            return Char{ch};
        }

        case KEY_CODE_YES:
            return Code{static_cast<int>(wch)};

        case ERR:
        default:
            return std::nullopt;
        }
    }
};

std::optional<wchar_t> printable(Key key)
{
    if (auto c = std::get_if<Char>(&key); c && std::iswprint(c->ch))
        return c->ch;
    return std::nullopt;
}

namespace key {
    inline constexpr Key ch(wchar_t c, bool meta = false) {
        return Char{c, meta};
    }

    inline constexpr Key ctrl(wchar_t c, bool meta = false) {
        return ch(static_cast<wchar_t>(c & 0x1f), meta);
    }

    inline constexpr Key fn(int curses_code) {
        return Code{curses_code};
    }

    inline constexpr Key esc() {
        return ch(L'\x1b');
    }
}

template<std::size_t Max>
struct KeySeq {
    std::array<Key, Max> keys{};
    std::size_t size = 0;

    constexpr void push(Key k)
    {
        if (size >= Max) throw "kbd sequence too long";
        keys[size++] = k;
    }

    constexpr operator std::span<const Key>() const
    { return {keys.data(), size}; }
};

constexpr char lower(char c)
{ return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

constexpr Key named_key(std::string_view name)
{
    struct Named {
        std::string_view name;
        Key key;
    };

    constexpr Named names[] = {
        {"esc",       key::esc()},
        {"escape",    key::esc()},
        {"tab",       key::ch(L'\t')},
        {"ret",       key::ch(L'\r')},
        {"return",    key::ch(L'\r')},
        {"space",     key::ch(L' ')},
        {"backspace", key::fn(KEY_BACKSPACE)},
        {"bs",        key::fn(KEY_BACKSPACE)},
        {"up",        key::fn(KEY_UP)},
        {"down",      key::fn(KEY_DOWN)},
        {"left",      key::fn(KEY_LEFT)},
        {"right",     key::fn(KEY_RIGHT)},
        {"home",      key::fn(KEY_HOME)},
        {"end",       key::fn(KEY_END)},
        {"pgup",      key::fn(KEY_PPAGE)},
        {"pgdown",    key::fn(KEY_NPAGE)},
        {"delete",    key::fn(KEY_DC)},
        {"del",       key::fn(KEY_DC)},
        {"insert",    key::fn(KEY_IC)},
        {"ins",       key::fn(KEY_IC)},
        {"resize",    key::fn(KEY_RESIZE)}
    };

    for (auto [n, k] : names)
        if (std::ranges::equal(name, n, {}, lower)) return k;

    if (name.size() >= 2 && lower(name[0]) == 'f') {
        int n = 0;

        for (char c: name.substr(1)) {
            if (c < '0' || c > '9') throw "unknown key name";

            n = n * 10 + c - '0';
        }

        if (n > 0) return key::fn(KEY_F(n));
    }

    throw "unknown key name";
}

constexpr bool starts_mod(std::string_view s, char mod)
{ return s.size() >= 2 && lower(s[0]) == lower(mod) && s[1] == '-'; }

constexpr bool is_space(char c)
{ return c == ' ' || c == '\t' || c == '\n'; }

constexpr void trim_left(std::string_view& s)
{ while (!s.empty() && is_space(s.front())) s.remove_prefix(1); }

template<std::size_t N>
consteval KeySeq<N> kbd(const char (&lit)[N])
{
    KeySeq<N> out;
    std::string_view s{lit, N - 1}; // Exclude terminating '\0'.

    while (trim_left(s), !s.empty()) {
        bool meta = false, ctrl = false;

        while (true) {
            if (starts_mod(s, 'M')) {
                meta = true;
                s.remove_prefix(2);
            } else if (starts_mod(s, 'C')) {
                ctrl = true;
                s.remove_prefix(2);
            } else {
                break;
            }
        }

        if (s.empty()) throw "missing key after modifier";

        Key base{};

        if (s.front() == '<' && s.contains('>')) {
            auto end = s.find('>');

            base = named_key(s.substr(1, end - 1));
            s.remove_prefix(end + 1);
        } else {
            char c = s.front();
            s.remove_prefix(1);

            base = key::ch(static_cast<wchar_t>(c));
        }

        if (ctrl) {
            if (auto chr = std::get_if<Char>(&base)) {
                base = key::ctrl(chr->ch, meta);
            } else throw "C- modifier only supported for character keys";
        } else if (meta) {
            if (auto chr = std::get_if<Char>(&base)) {
                chr->meta = true;
            } else throw "M- modifier only supported for character keys";
        }

        out.push(base);
    }

    return out;
}

static_assert(std::get<Char>(kbd("M-<").keys[0]) == Char{L'<', true});
static_assert(std::get<Char>(kbd("M->").keys[0]) == Char{L'>', true});
static_assert(std::get<Code>(kbd("<left>").keys[0]) == Code{KEY_LEFT});

template<class Ctx>
struct Keymap
{
    using Action = std::function<void(Ctx&)>;
    using DefaultAction = std::function<bool(Ctx&, Key)>;

    struct Binding;              // incomplete

    std::vector<Binding> bindings;
    DefaultAction default_action;

    Keymap();
    Keymap(std::initializer_list<Binding>);
    Keymap(DefaultAction, std::initializer_list<Binding> = {});

    Keymap(const Keymap&);
    Keymap& operator=(const Keymap&);

    Keymap(Keymap&&) noexcept;
    Keymap& operator=(Keymap&&) noexcept;

    ~Keymap();

    void define_default(DefaultAction def)
    { default_action = std::move(def); }

    bool run_default(Key k, Ctx& ctx) const
    { return default_action && default_action(ctx, k); }

    const Binding* find(Key k) const;
    Binding* find(Key k);

    void set(std::span<const Key> seq, Action action)
    { set_impl(seq, std::move(action)); }

    void set(std::span<const Key> seq, Keymap nested)
    { set_impl(seq, std::move(nested)); }

private:
    static Keymap& child_map(Binding&);
    static Binding& prefix_binding(Keymap&, Key);

    template<class Target>
    void set_impl(std::span<const Key> seq, Target target);
};

template<class Ctx>
struct Keymap<Ctx>::Binding
{
    using Target = std::variant<Action, Keymap>;

    Key key;
    Target target;

    Binding() = default;

    Binding(Key k, Action a) : key{k}, target{std::move(a)} {}
    Binding(Key k, Keymap nested) : key{k}, target{std::move(nested)} {}
};

template<class Ctx>
Keymap<Ctx>::Keymap() = default;

template<class Ctx>
Keymap<Ctx>::Keymap(std::initializer_list<Binding> bs)
: bindings{bs}
{}

template<class Ctx>
Keymap<Ctx>::Keymap(DefaultAction def, std::initializer_list<Binding> bs)
: bindings{bs}, default_action{std::move(def)}
{}

template<class Ctx>
Keymap<Ctx>::Keymap(const Keymap&) = default;

template<class Ctx>
Keymap<Ctx>& Keymap<Ctx>::operator=(const Keymap&) = default;

template<class Ctx>
Keymap<Ctx>::Keymap(Keymap&&) noexcept = default;

template<class Ctx>
Keymap<Ctx>& Keymap<Ctx>::operator=(Keymap&&) noexcept = default;

template<class Ctx>
Keymap<Ctx>::~Keymap() = default;

template<class Ctx>
const typename Keymap<Ctx>::Binding* Keymap<Ctx>::find(Key k) const
{
    auto iter = std::ranges::find(bindings, k, &Binding::key);
    return iter == bindings.end() ? nullptr : &*iter;
}

template<class Ctx>
typename Keymap<Ctx>::Binding* Keymap<Ctx>::find(Key k)
{
    auto iter = std::ranges::find(bindings, k, &Binding::key);
    return iter == bindings.end() ? nullptr : &*iter;
}

template<class Ctx>
Keymap<Ctx>& Keymap<Ctx>::child_map(Binding& b)
{
    if (auto* next = std::get_if<Keymap<Ctx>>(&b.target))
        return *next;

    return b.target.template emplace<Keymap<Ctx>>();
}

template<class Ctx>
typename Keymap<Ctx>::Binding& Keymap<Ctx>::prefix_binding(Keymap& map, Key k)
{
    if (auto* b = map.find(k)) return *b;

    map.bindings.emplace_back(k, Keymap{});
    return map.bindings.back();
}

template<class Ctx>
template<class Target>
void Keymap<Ctx>::set_impl(std::span<const Key> seq, Target target)
{
    if (seq.empty()) return;

    Keymap* map = this;

    for (Key k: seq.subspan(0, seq.size() - 1))
        map = &child_map(prefix_binding(*map, k));

    Key last = seq.back();

    if (Binding* b = map->find(last)) {
        *b = Binding{last, std::move(target)};
    } else {
        map->bindings.emplace_back(last, std::move(target));
    }
}

template<class Ctx>
struct KeyDispatcher
{
    enum class Result { accepted, prefix, rejected };

    explicit KeyDispatcher(const Keymap<Ctx>& active)
    : active_(&active)
    {}

    void set_active_keymap(const Keymap<Ctx>& active)
    {
        active_ = &active;
        reset();
    }

    const Keymap<Ctx>& active_keymap() const
    { return *active_; }

    Result feed(Key k, Ctx& ctx)
    {
        const Keymap<Ctx>& map = current_map();

        const auto* b = map.find(k);

        if (!b) {
            bool handled = map.run_default(k, ctx);
            reset();

            return handled ? Result::accepted : Result::rejected;
        }

        if (auto* next = std::get_if<Keymap<Ctx>>(&b->target)) {
            pending_ = next;
            return Result::prefix;
        }

        const auto& action = std::get<typename Keymap<Ctx>::Action>(b->target);

        if (!action) {
            reset();
            return Result::rejected;
        }

        action(ctx);
        reset();
        return Result::accepted;
    }

    void reset()
    {
        pending_ = nullptr;
    }

private:
    const Keymap<Ctx>& current_map() const
    { return pending_ ? *pending_ : *active_; }

    const Keymap<Ctx>* active_;
    const Keymap<Ctx>* pending_ = nullptr;
};

} // namespace mlang::curses
