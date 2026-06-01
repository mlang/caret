#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace mlang {

struct Syntax
{
    enum class Language;
    static std::span<const Language> available_languages() noexcept;
    friend std::string_view canonical_name(Language) noexcept;
    static std::optional<Language> language_from(std::string_view) noexcept;
    static std::optional<Language> language_from(std::filesystem::path);
    static std::optional<Language> language_from_shebang(std::u8string_view) noexcept;

    /// Parse `source` as `language`; source text is not retained.
    Syntax(std::u8string_view source, Language language);
    ~Syntax();
    Syntax(Syntax&&) noexcept;
    Syntax& operator=(Syntax&&) noexcept;

    Syntax(const Syntax&) = delete;
    Syntax& operator=(const Syntax&) = delete;

    /// True when all layers have complete parse trees.
    [[nodiscard]] explicit operator bool() const noexcept;

    /// Continue any timed-out parses using the current full source text.
    /// Returns true when parsing is complete.
    bool continue_parsing(std::u8string_view source);

    struct ByteIndex
    {
        unsigned int index = 0;

        friend auto operator<=>(ByteIndex, ByteIndex) = default;
    };

    struct Point
    {
        unsigned int row = 0, byte_column = 0;

        friend auto operator<=>(Point, Point) = default;
    };

    struct Position
    {
        ByteIndex byte;
        Point point;

        friend auto operator<=>(Position, Position) = default;
    };

    template<class T>
    struct Range
    {
        T start;
        T end;

        friend auto operator<=>(const Range&, const Range&) = default;

        [[nodiscard]] bool empty() const noexcept
        { return !(start < end); }

        [[nodiscard]] bool contains(T value) const noexcept
        { return start <= value && value < end; }

        [[nodiscard]] bool contains(Range other) const noexcept
        { return start <= other.start && other.end <= end; }

        [[nodiscard]] bool intersects(Range other) const noexcept
        { return start < other.end && other.start < end; }
    };

    using ByteRange = Range<ByteIndex>;
    using PointRange = Range<Point>;
    using Region = Range<Position>;

    // Node is a transient view.
    // Invalidated by any mutation of Syntax.
    struct Node
    {
        Language language() const noexcept;
        std::string_view type() const noexcept;
        bool is_named() const noexcept;
        std::u8string_view text(std::u8string_view source) const noexcept;
        Region range() const noexcept;

        std::optional<Node> parent() const;
        std::optional<Node> next_sibling() const;
        std::optional<Node> previous_sibling() const;
        std::optional<Node> next_named_sibling() const;
        std::optional<Node> previous_named_sibling() const;

        struct Children;
        Children children() const noexcept;
        Children named_children() const noexcept;

        Node(const Node&) noexcept;
        Node& operator=(const Node&) noexcept;
        Node(Node&&) noexcept;
        Node& operator=(Node&&) noexcept;
        ~Node() noexcept;

        struct Factory;

    private: // small pimpl
        struct Impl;
        friend class Factory;
        explicit Node(Impl&&) noexcept;

        Impl* impl() noexcept;
        const Impl* impl() const noexcept;

        static constexpr std::size_t impl_size = 40;
        static constexpr std::size_t impl_align = alignof(std::max_align_t);

        alignas(impl_align) std::byte storage[impl_size];
    };

    [[nodiscard]] std::optional<Node> root() const noexcept;

    /// Smallest node in the innermost layer containing `range`.
    /// An empty range is treated as a point lookup.
    std::optional<Node> descendant_for(ByteRange) const;
    std::optional<Node> descendant_for(PointRange) const;
    std::optional<Node> named_descendant_for(ByteRange) const;
    std::optional<Node> named_descendant_for(PointRange) const;

    struct Edit
    {
        Region old;
        Position new_end;
    };

    /// Apply edit(s) and reparse affected layers.
    /// Edits use old-source coordinates for the buffer state before each edit;
    /// `new_source` is the full final buffer.
    bool edit(std::u8string_view new_source, Edit edit);
    bool edit(std::u8string_view new_source, std::span<const Edit> edits);

    void dump_layers(std::ostream& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

enum class Syntax::Language
{
    ADL,
    Ada,
    Agda,
    Alloy,
    Amber,
    Astro,
    Awk,
    Bash,
    Basic,
    Bass,
    Batch,
    Beancount,
    BibTeX,
    Bicep,
    BitBake,
    Blade,
    Blueprint,
    C,
    C3,
    CEL,
    CMake,
    CPON,
    CPlusPlus,
    CSS,
    CSV,
    CSharp,
    Caddyfile,
    Cairo,
    CapNProto,
    ChucK,
    Circom,
    Clarity,
    Clojure,
    Comment,
    CommonLisp,
    Crystal,
    Cue,
    Cylc,
    Cython,
    D,
    DBML,
    DTD,
    Dart,
    Debian,
    DeviceTree,
    Dhall,
    Diff,
    Djot,
    Dockerfile,
    Dot,
    Doxyfile,
    Doxygen,
    Drools,
    Dunstrc,
    EBNF,
    EEX,
    EmacsLisp,
    Earthfile,
    Eiffel,
    Elixir,
    Elm,
    Elvish,
    EmbeddedTemplate,
    Erlang,
    FGA,
    FIDL,
    FSharp,
    Fennel,
    Fish,
    FlatBuffers,
    Forth,
    Fortran,
    FreeBasic,
    GAS,
    GDScript,
    GLSL,
    GN,
    GPR,
    Gemini,
    Gherkin,
    Ghostty,
    GitAttributes,
    GitCommit,
    GitConfig,
    GitIgnore,
    GitRebase,
    Gleam,
    Glimmer,
    GlimmerJavaScript,
    GlimmerTypeScript,
    Gnuplot,
    Go, GoFormatString,
    GodotResource,
    GraphQL,
    Gren,
    Groovy,
    HCL,
    HDL,
    HEEX,
    HOCON,
    HTML,
    HTMLDjango,
    Hare,
    Haskell, HaskellLiterate, HaskellPersistent,
    Haxe,
    Hoon,
    Hosts,
    Hurl,
    Hyprlang,
    IEX,
    INI,
    Ink,
    Inko,
    JJDescription,
    JJRevset,
    JJTemplate,
    JQ,
    JSDoc,
    JSON,
    JSON5,
    JanetSimple,
    Java,
    JavaScript,
    Jinja2,
    Jsonnet,
    Julia,
    Just,
    KCL,
    KConfig,
    KDL,
    Klog,
    Koka,
    Kotlin,
    Koto,
    LD,
    LDIF,
    LPF,
    LaTeX,
    Lean,
    Ledger,
    Less,
    Log,
    Lua, LuaFormatString, LuaPattern,
    Luau,
    MATLAB,
    Mail,
    Make,
    Markdoc,
    Markdown, MarkdownInline, MarkdownRustdoc,
    Mermaid,
    Meson,
    Mojo,
    MoonBit,
    Move,
    NASM,
    Nearley,
    Nginx,
    Nickel,
    Nim,
    Nix,
    Nu,
    OHM,
    Odin,
    OpenCL,
    OpenSCAD,
    Org,
    P,
    PEM,
    PHP,
    PHPOnly,
    PKL,
    PO,
    PRQL,
    PTX,
    Pascal,
    Passwd,
    Penrose,
    Perl, PlainOldDocumentation,
    Picat,
    PonyLang,
    PowerShell,
    Prisma,
    ProVerif,
    Prolog,
    Properties,
    Proto,
    Pug,
    Puppet,
    PureScript,
    Python,
    QL,
    QMLJS,
    R,
    RON,
    RPMBash,
    RPMSpec,
    RSHTML,
    ReScript,
    ReStructuredText,
    Rego,
    RegularExpression,
    Requirements,
    Robot,
    RobotsTxt,
    Ruby,
    Rust, RustFormatArgs, RustFormatArgsMacro,
    SCFG,
    SCSS,
    SlightLisp,
    SML,
    SQL,
    SSHClientConfig,
    Scala,
    Scheme,
    ShellCheckRC,
    Slang,
    Slint,
    Smali,
    Smithy,
    Snakemake,
    Solidity,
    SourcePawn,
    Spade,
    SpiceDB,
    Strace,
    StrictDoc,
    SuperCollider,
    Sway,
    Swift,
    SystemVerilog,
    T32,
    TCL,
    TLAPlus,
    TOML,
    TQL,
    TableGen,
    Tact,
    Task,
    Teal,
    Templ,
    Tera,
    TextProto,
    Thrift,
    TodoTxt,
    TreeSitterQuery,
    Twig,
    TypeScript, TSX,
    TypeSpec,
    Typst,
    Ungrammar,
    Unison,
    Uxntal,
    VHDL,
    VHS,
    Vala,
    Vento,
    Verilog,
    Vim,
    WESL,
    WGSL,
    Werk,
    WikiText,
    Wren,
    XML,
    XTC,
    Xit,
    YAML,
    YARA,
    Yuck,
    Zig
};

std::string_view canonical_name(Syntax::Language) noexcept;

struct Syntax::Node::Children
: std::ranges::view_interface<Syntax::Node::Children>
{
    class iterator;

    iterator begin() const noexcept;
    iterator end() const noexcept;

    std::size_t size() const noexcept;

private:
    friend struct Node;

    Node node;
    bool named = false;

    Children(Node node, bool named) noexcept;
};

class Syntax::Node::Children::iterator
{
    friend struct Children;

    const Children* owner = nullptr;
    std::ptrdiff_t index = 0;

    iterator(const Children* owner, std::ptrdiff_t index) noexcept;

public:
    using iterator_concept = std::random_access_iterator_tag;
    using value_type = Node;
    using difference_type = std::ptrdiff_t;

    iterator() noexcept = default;

    Node operator*() const noexcept;
    Node operator[](difference_type n) const noexcept;

    iterator& operator++() noexcept;
    iterator operator++(int) noexcept;

    iterator& operator--() noexcept;
    iterator operator--(int) noexcept;

    iterator& operator+=(difference_type n) noexcept;
    iterator& operator-=(difference_type n) noexcept;

    friend iterator operator+(iterator it, difference_type n) noexcept
    {
        it += n;
        return it;
    }

    friend iterator operator+(difference_type n, iterator it) noexcept
    {
        it += n;
        return it;
    }

    friend iterator operator-(iterator it, difference_type n) noexcept
    {
        it -= n;
        return it;
    }

    friend difference_type operator-(iterator a, iterator b) noexcept
    {
        return a.index - b.index;
    }

    friend bool operator==(iterator a, iterator b) noexcept
    {
        return a.owner == b.owner && a.index == b.index;
    }

    friend auto operator<=>(iterator a, iterator b) noexcept
    {
        //assert(a.owner == b.owner);
        return a.index <=> b.index;
    }
};

} // namespace mlang
