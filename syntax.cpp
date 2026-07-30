#include <mlang/syntax.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <new>
#include <ranges>
#include <regex>
#include <span>
#include <stdexcept>
#include <vector>

#include <tree_sitter/api.h>

namespace mlang {

namespace {

using LanguageFunction = const TSLanguage *();

struct LanguageInfo
{
    Syntax::Language language;
    std::string_view name;
    LanguageFunction *make;
};

const auto injections_query_source = []{
    std::array<std::string, static_cast<std::size_t>(Syntax::Language::Zig) + 1> queries;
    auto q = [&](Syntax::Language lang) -> std::string& {
        return queries[static_cast<std::size_t>(lang)];
    };
    using enum Syntax::Language;

    q(Awk) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

((regex_pattern) @injection.content
 (#set! injection.language "regex"))
)query";

    q(Bash) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

(command
  name: (command_name (word) @_command)
  argument: (raw_string) @injection.content
 (#match? @_command "^[gnm]?awk$")
 (#set! injection.language "awk"))

((regex) @injection.content
  (#set! injection.language "regex"))

(command
  name: (command_name (word) @_command (#any-of? @_command "jq" "jaq"))
  argument: [
    (raw_string) @injection.content
    (string (string_content) @injection.content)
  ]
  (#set! injection.language "jq"))

(command
  name: (command_name (word) @_command (#eq? @_command "alias"))
  argument: (concatenation
    (word)
    [
      (raw_string) @injection.content
      (string (string_content) @injection.content)
    ])
  (#set! injection.language "bash"))

(command
  name: (command_name (word) @_command (#any-of? @_command "eval" "trap"))
  .
  argument: [
    (raw_string) @injection.content
    (string (string_content) @injection.content)
  ]
  (#set! injection.language "bash"))
)query";

    q(Blade) = q(HTML) + R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

((php_only) @injection.content
    (#set! injection.language "php-only"))

((parameter) @injection.content
    (#set! injection.include-children) ; You may need this, depending on your editor e.g Helix
    (#set! injection.language "php-only"))

; ; Livewire attributes
(attribute
  (attribute_name) @_attr
    (#any-of? @_attr
      "wire:click"
      "wire:submit"
      "wire:model"
      "wire:loading"
      "wire:navigate"
      "wire:current"
      "wire:cloak"
      "wire:dirty"
      "wire:confirm"
      "wire:transition"
      "wire:init"
      "wire:poll"
      "wire:offline"
      "wire:ignore"
      "wire:replace"
      "wire:show"
      "wire:stream"
      "wire:text"
    )
  (quoted_attribute_value
    (attribute_value) @injection.content)
  (#set! injection.language "javascript"))

; ; AlpineJS attributes
(attribute
  (attribute_name) @_attr
    (#match? @_attr "^x-[a-z]+")
  (quoted_attribute_value
    (attribute_value) @injection.content)
  (#set! injection.language "javascript"))

; ; Apline Events
(attribute
  (attribute_name) @_attr
    (#match? @_attr "^@[a-z]+")
  (quoted_attribute_value
    (attribute_value) @injection.content)
  (#set! injection.language "javascript"))

; ; normal HTML element alpine attributes
(element
  (_
    (tag_name) @_tag
      (#match? @_tag "[^x][^-]")
    (attribute
      (attribute_name) @_attr
        (#match? @_attr "^:[a-z]+")
      (quoted_attribute_value
        (attribute_value) @injection.content)
      (#set! injection.combined)
      (#set! injection.language "javascript"))))

; ; ; Blade escaped JS attributes
; ; <x-foo ::bar="baz" />
(element
  (_
    (tag_name) @_tag
      (#match? @_tag "^x-[a-z]+")
    (attribute
      (attribute_name) @_attr
        (#match? @_attr "^::[a-z]+")
      (quoted_attribute_value
        (attribute_value) @injection.content)
      (#set! injection.language "javascript"))))

; ; ; Blade escaped JS attributes
; ; <htmlTag :class="baz" />
(element
  (_
    (attribute_name) @_attr
      (#match? @_attr "^:[a-z]+")
    (quoted_attribute_value
      (attribute_value) @injection.content)
    (#set! injection.language "javascript")))

; Blade PHP attributes
(element
  (_
    (tag_name) @_tag
      (#match? @_tag "^x-[a-z]+")
    (attribute
      (attribute_name) @_attr
        (#match? @_attr "^:[a-z]+")
      (quoted_attribute_value
        (attribute_value) @injection.content)
      (#set! injection.language "php-only"))))
)query";

    q(C) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

((preproc_arg) @injection.content
 (#set! injection.language "c")
 (#set! injection.include-children))
)query";

    q(CPlusPlus) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

((preproc_arg) @injection.content
 (#set! injection.language "cpp")
 (#set! injection.include-children))

(raw_string_literal
  delimiter: (raw_string_delimiter) @injection.language
  (raw_string_content) @injection.content)
)query";

    q(CommonLisp) = R"query(
([(comment) (block_comment)] @injection.content
 (#set! injection.language "comment"))
)query";

    q(Crystal) = R"query(
((heredoc_body
  (literal_content) @injection.content
  (heredoc_end) @name
  (#set! injection.language "sql"))
  (#eq? @name "SQL"))

((heredoc_body
  (literal_content) @injection.content
  (heredoc_end) @name
  (#set! injection.language "html"))
  (#eq? @name "HTML"))
)query";

    q(CSharp) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(CSS) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(D) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(Djot) = R"query(
(comment (content) @injection.content
  (#set! injection.language "comment"))

(math (content) @injection.content
  (#set! injection.language "latex") (#set! injection.include-unnamed-children))

(code_block
  (language) @injection.language
  (code) @injection.content (#set! injection.include-unnamed-children))

(raw_block
  (raw_block_info
    (language) @injection.language)
  (content) @injection.content (#set! injection.include-unnamed-children))

(raw_inline
  (content) @injection.content (#set! injection.include-unnamed-children)
  (raw_inline_attribute
    (language) @injection.language))
)query";

    q(FSharp) = R"query(
([
 (line_comment)
 (block_comment_content)
] @injection.content
  (#set! injection.language "comment"))

((xml_doc) @injection.content
 (#set! injection.language "xml"))
)query";

    q(Fortran) = R"query(
((comment) @injection.content
  (#set! injection.language "comment"))
)query";

    q(Go) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

; Inject markdown into documentation comments
;
; Go's comments are documentation comments when they are directly followed
; by one of Go's statements (e.g. `type`, `func`, `const`)
;
; This is only a partial implementation, which covers only
; block comments. For line comments (which are more common),
; upstream changes to the grammar are required.
(source_file
  (comment) @injection.content . (comment)* . [
    (package_clause) ; `package`
    (type_declaration) ; `type`
    (function_declaration) ; `func`
    (method_declaration) ; `func`
    (var_declaration) ; `var`
    (const_declaration) ; `const`
    ; var (
    ; 	A = 1
    ; 	B = 2
    ; )
    (var_spec)
    ; const (
    ; 	A = 1
    ; 	B = 2
    ; )
    (const_spec)
  ]
  (#set! injection.language "markdown"))

((comment) @injection.content
 (#match? @injection.content "^//go:generate")
 (#set! injection.language "bash"))

(call_expression
  (selector_expression) @_function
  (#any-of? @_function "regexp.Match" "regexp.MatchReader" "regexp.MatchString" "regexp.Compile" "regexp.CompilePOSIX" "regexp.MustCompile" "regexp.MustCompilePOSIX")
  (argument_list
    .
    [
      (raw_string_literal (raw_string_literal_content) @injection.content)
      (interpreted_string_literal (interpreted_string_literal_content) @injection.content)
    ]
    (#set! injection.language "regex")))

; https://pkg.go.dev/fmt#Printf
; https://pkg.go.dev/fmt#Sprintf
; https://pkg.go.dev/fmt#Scanf
; https://pkg.go.dev/fmt#Errorf
((call_expression
  function: (selector_expression
    operand: (identifier) @_module
    field: (field_identifier) @_func)
  arguments: (argument_list
    . (interpreted_string_literal) @injection.content))
  (#eq? @_module "fmt")
  (#any-of? @_func "Printf" "Sprintf" "Scanf" "Errorf")
  (#set! injection.language "go-format-string"))

; https://pkg.go.dev/fmt#Fprintf
; https://pkg.go.dev/fmt#Fscanf
; https://pkg.go.dev/fmt#Sscanf
((call_expression
  function: (selector_expression
    operand: (identifier) @_module
    field: (field_identifier) @_func)
  arguments: (argument_list
    ; [(identifier) (interpreted_string_literal)]
    (_)
    ; (identifier)
    .
    (interpreted_string_literal) @injection.content))
  (#eq? @_module "fmt")
  (#any-of? @_func "Fprintf" "Fscanf" "Sscanf")
  (#set! injection.language "go-format-string"))

; https://pkg.go.dev/log#Printf
; https://pkg.go.dev/log#Fatalf
; https://pkg.go.dev/log#Panicf
; https://pkg.go.dev/log#Logger.Printf
; https://pkg.go.dev/log#Logger.Fatalf
; https://pkg.go.dev/log#Logger.Panicf
((call_expression
  function: (selector_expression
    operand: (identifier)
    field: (field_identifier) @_func)
  arguments: (argument_list
    . (interpreted_string_literal) @injection.content))
  (#any-of? @_func "Printf" "Fatalf" "Panicf")
  (#set! injection.language "go-format-string"))
)query";

    q(HaskellLiterate) = R"query(
; Inject Haskell parser into bird-style code lines
((bird_line
  (haskell_code) @injection.content)
 (#set! injection.language "haskell"))

; Inject Haskell parser into LaTeX code blocks
((latex_code_line
  (haskell_code) @injection.content)
 (#set! injection.language "haskell"))

; Inject Haskell parser into Markdown code blocks
((markdown_code_line
  (haskell_code) @injection.content)
 (#set! injection.language "haskell"))
)query";

    q(HTML) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

((script_element
  (raw_text) @injection.content)
 (#set! injection.language "javascript"))

((style_element
  (raw_text) @injection.content)
 (#set! injection.language "css"))
)query";

    q(JQ) = R"query(
((comment) @injection.content
  (#set! injection.language "comment"))

; test(val)
(query
  ((funcname) @_function
    (#any-of? @_function "test" "match" "capture" "scan" "split" "splits" "sub" "gsub"))
  (args
    .
    (query
      (string) @injection.content
      (#set! injection.language "regex"))))

; test(regex; flags)
(query
  ((funcname) @_function
    (#any-of? @_function "test" "match" "capture" "scan" "split" "splits" "sub" "gsub"))
  (args
    .
    (args
      (query
        (string) @injection.content
        (#set! injection.language "regex")))))
)query";

    q(Julia) = R"query(
(
  (source_file
    (string_literal) @injection.content
    .
    [
      (module_definition)
      (function_definition)
      (macro_definition)
      (primitive_definition)
      (abstract_definition)
      (struct_definition)
      (assignment)
      (const_statement)
    ])
  (#set! injection.language "markdown"))

(
  [
    (line_comment)
    (block_comment)
  ] @injection.content
  (#set! injection.language "comment"))

(
  [
    (command_literal)
    (prefixed_command_literal)
  ] @injection.content
  (#set! injection.language "bash"))

(
  (prefixed_string_literal
    prefix: (identifier) @function.macro) @injection.content
  (#eq? @function.macro "r")
  (#set! injection.language "regex"))

(
  (prefixed_string_literal
    prefix: (identifier) @function.macro) @injection.content
  (#eq? @function.macro "md")
  (#set! injection.language "markdown"))
)query";

    q(Kotlin) = R"query(
([
  (line_comment)
  (multiline_comment)
 ] @injection.content
	(#set! injection.language "comment"))

; There are 3 ways to define a regex
;    - "[abc]?".toRegex()
((call_expression
	(navigation_expression
		(string_literal
		   (string_content) @injection.content)
		(navigation_suffix
			((simple_identifier) @_function
			(#eq? @_function "toRegex")))))
	(#set! injection.language "regex"))

;    - Regex("[abc]?")
((call_expression
	((simple_identifier) @_function
	(#eq? @_function "Regex"))
	(call_suffix
		(value_arguments
			(value_argument
				(string_literal
					(string_content) @injection.content)))))
	(#set! injection.language "regex"))

;    - Regex.fromLiteral("[abc]?")
((call_expression
	(navigation_expression
		((simple_identifier) @_class
		(#eq? @_class "Regex"))
		(navigation_suffix
			((simple_identifier) @_function
			(#eq? @_function "fromLiteral"))))
	(call_suffix
		(value_arguments
			(value_argument
				(string_literal
					(string_content) @injection.content)))))
	(#set! injection.language "regex"))
)query";

    q(LaTeX) = R"query(
([
   (comment)
   (line_comment)
   (block_comment)
   (comment_environment)
 ] @injection.content (#set! injection.language "comment"))
)query";

    q(Lean) = R"query(
((comment) @injection.content
 (#set! injection.language "markdown"))
)query";

    q(Lua) = R"query(
((comment) @injection.content
 (#set! injection.language "comment")
 (#set! injection.include-children))

; string.match("123", "%d+")
(function_call
  (dot_index_expression
    field: (identifier) @_method
    (#any-of? @_method "find" "match" "gmatch" "gsub"))
  arguments: (arguments
    .
    (_)
    .
    (string
      content: (string_content) @injection.content
      (#set! injection.language "luap")
      (#set! injection.include-children))))

; ("123"):match("%d+")
(function_call
  (method_index_expression
    method: (identifier) @_method
    (#any-of? @_method "find" "match" "gmatch" "gsub"))
  arguments: (arguments
    .
    (string
      content: (string_content) @injection.content
      (#set! injection.language "luap")
      (#set! injection.include-children))))

; string.format("format string", ...)
((function_call
  name: (dot_index_expression
    table: (identifier) @_table
    field: (identifier) @_function)
  arguments: (arguments
    .
    (string
      content: (string_content) @injection.content)))
  (#eq? @_table "string")
  (#eq? @_function "format")
  (#set! injection.language "lua-format-string"))

; ("format"):format(...)
((function_call
  name: (method_index_expression
    table: (parenthesized_expression
      (string
        content: (string_content) @injection.content))
    method: (identifier) @_function))
  (#eq? @_function "format")
  (#set! injection.language "lua-format-string"))
)query";

    q(Markdown) = R"query(
(fenced_code_block
  (code_fence_content) @injection.shebang @injection.content
  (#set! injection.include-unnamed-children))

(fenced_code_block
  (info_string
    (language) @injection.language)
  (code_fence_content) @injection.content (#set! injection.include-unnamed-children))

((html_block) @injection.content
 (#set! injection.language "html")
 (#set! injection.include-unnamed-children)
 (#set! injection.combined))

((pipe_table_cell) @injection.content (#set! injection.language "markdown.inline") (#set! injection.include-unnamed-children))

((minus_metadata) @injection.content (#set! injection.language "yaml") (#set! injection.include-unnamed-children))
((plus_metadata) @injection.content (#set! injection.language "toml") (#set! injection.include-unnamed-children))

((inline) @injection.content (#set! injection.language "markdown.inline") (#set! injection.include-unnamed-children))
)query";

    // In Rust, it is common to have documentation code blocks not specify the
    // language, and it is assumed to be Rust if it is not specified.
    q(MarkdownRustdoc) = q(Markdown) + R"query(
(fenced_code_block
  (code_fence_content) @injection.content
  (#set! injection.language "rust")
  (#set! injection.include-unnamed-children))

(fenced_code_block
  (info_string
    (language) @injection.language)
  (code_fence_content) @injection.content (#set! injection.include-unnamed-children))

(fenced_code_block
  (info_string
    (language) @__language)
  (code_fence_content) @injection.content
  ; list of attributes for Rust syntax highlighting:
  ; https://doc.rust-lang.org/rustdoc/write-documentation/documentation-tests.html#attributes
  (#match? @__language
  "(ignore|should_panic|no_run|compile_fail|standalone_crate|custom|edition*)")
  (#set! injection.language "rust")
  (#set! injection.include-unnamed-children))
)query";

    q(Nim) = R"query(
([(comment (comment_content) @injection.content)
  (block_comment (comment_content) @injection.content)]
 (#set! injection.language "comment"))
)query";

    q(Nix) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

((((comment) @injection.language) .
  (indented_string_expression (string_fragment) @injection.content))
 (#set! injection.combined))

; nixos testScript binding - value is Python.
((binding
   attrpath: (attrpath (identifier) @_path)
   expression: (indented_string_expression
     (string_fragment) @injection.content))
 (#eq? @_path "testScript")
 (#set! injection.language "python")
 (#set! injection.combined))

; nixos testScript binding with `let ... in ''...''` - value is Python.
((binding
   attrpath: (attrpath (identifier) @_path)
   expression: (let_expression
     body: (indented_string_expression
       (string_fragment) @injection.content)))
 (#eq? @_path "testScript")
 (#set! injection.language "python")
 (#set! injection.combined))

; Common binding-name -> bash injections.
; Covers Phase/Hook/Script conventions used across nixpkgs stdenv.
((binding
   attrpath: (attrpath (identifier) @_path)
   expression: [
     (indented_string_expression (string_fragment) @injection.content)
     (binary_expression (indented_string_expression (string_fragment) @injection.content))
   ])
 (#match? @_path "(^\\w*Phase|command|(pre|post)\\w*|(.*\\.)?\\w*([sS]cript|[hH]ook)|(.*\\.)?startup)$")
 (#set! injection.language "bash")
 (#set! injection.combined))

; builtins.{match,split} regex str
; Example: nix/tests/lang/eval-okay-regex-{match,split}.nix
((apply_expression
   function: (_) @_func
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)(match|split)$")
 (#set! injection.language "regex")
 (#set! injection.combined))

; builtins.fromJSON json
; Example: nix/tests/lang/eval-okay-fromjson.nix
((apply_expression
   function: (_) @_func
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)fromJSON$")
 (#set! injection.language "json")
 (#set! injection.combined))

; builtins.fromTOML toml
; Example: nix/tests/functional/lang/eval-okay-fromTOML.nix
((apply_expression
   function: (_) @_func
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)fromTOML$")
 (#set! injection.language "toml")
 (#set! injection.combined))

; pkgs.writeShellScript / writeShellScriptBin - 2nd argument is bash.
((apply_expression
   function: (apply_expression function: (_) @_func)
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)writeShellScript(Bin)?$")
 (#set! injection.language "bash")
 (#set! injection.combined))

; pkgs.runCommand variants - 3rd positional argument is bash.
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)runCommand(((No)?(CC))?(Local)?)?$")
  (#set! injection.language "bash")
  (#set! injection.combined))

; pkgs.writeShellApplication - the `text` attribute is bash.
(apply_expression
  function: ((_) @_func)
  argument: (_ (_)* (_ (_)* (binding
    attrpath: (attrpath (identifier) @_path)
    expression: (indented_string_expression
      (string_fragment) @injection.content))))
  (#match? @_func "(^|\\.)writeShellApplication$")
  (#match? @_path "^text$")
  (#set! injection.language "bash")
  (#set! injection.combined))

; writeShellApplication with `text = let ... in "..."` - follow the let body.
(apply_expression
  function: ((_) @_func)
  argument: (_ (_)* (_ (_)* (binding
    attrpath: (attrpath (identifier) @_path)
    expression: (let_expression
      body: (indented_string_expression
        (string_fragment) @injection.content)))))
  (#match? @_func "(^|\\.)writeShellApplication$")
  (#match? @_path "^text$")
  (#set! injection.language "bash")
  (#set! injection.combined))

; lib.literalExpression / lib.literalExpressionPrefix - the string
; argument is a Nix expression shown in docs; highlight as nix.
; Uses specific node-type alternation rather than (_) to avoid
; interference with other query captures.
((apply_expression
   function: [
     (variable_expression (identifier) @_func)
     (select_expression attrpath: (attrpath attr: (identifier) @_func .))
   ]
   argument: (indented_string_expression
     (string_fragment) @injection.content))
 (#match? @_func "^literalExpression(Prefix)?$")
 (#set! injection.language "nix")
 (#set! injection.combined))

((apply_expression
   function: [
     (variable_expression (identifier) @_func)
     (select_expression attrpath: (attrpath attr: (identifier) @_func .))
   ]
   argument: (string_expression
     (string_fragment) @injection.content))
 (#match? @_func "^literalExpression(Prefix)?$")
 (#set! injection.language "nix"))

; pkgs.writeCBin name content
((apply_expression
   function: (apply_expression function: (_) @_func)
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)writeC(Bin)?$")
 (#set! injection.language "c")
 (#set! injection.combined))

; pkgs.writers.write{Bash,Dash}[Bin] name [attrs] content
((apply_expression
   function: (apply_expression function: [
     ((_) @_func)
     (apply_expression function: (_) @_func)
   ])
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)write[BD]ash(Bin)?$")
 (#set! injection.language "bash")
 (#set! injection.combined))

; pkgs.writers.writeFish[Bin] name [attrs] content
((apply_expression
   function: (apply_expression function: [
     ((_) @_func)
     (apply_expression function: (_) @_func)
   ])
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)writeFish(Bin)?$")
 (#set! injection.language "fish")
 (#set! injection.combined))

; pkgs.writers.* usage examples: nixpkgs/pkgs/build-support/writers/test.nix

; pkgs.writers.writeRust[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeRust(Bin)?$")
  (#set! injection.language "rust")
  (#set! injection.combined))

; pkgs.writers.writeHaskell[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeHaskell(Bin)?$")
  (#set! injection.language "haskell")
  (#set! injection.combined))

; pkgs.writers.writeNim[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeNim(Bin)?$")
  (#set! injection.language "nim")
  (#set! injection.combined))

; pkgs.writers.writeJS[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeJS(Bin)?$")
  (#set! injection.language "javascript")
  (#set! injection.combined))

; pkgs.writers.writePerl[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writePerl(Bin)?$")
  (#set! injection.language "perl")
  (#set! injection.combined))

; pkgs.writers.write{Python,PyPy}{2,3}[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)write(Python|PyPy)[23](Bin)?$")
  (#set! injection.language "python")
  (#set! injection.combined))

; pkgs.writers.writeNu[Bin] name [attrs] content
((apply_expression
   function: (apply_expression function: [
     ((_) @_func)
     (apply_expression function: (_) @_func)
   ])
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)writeNu(Bin)?$")
 (#set! injection.language "nu")
 (#set! injection.combined))

; pkgs.writers.writeRuby[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeRuby(Bin)?$")
  (#set! injection.language "ruby")
  (#set! injection.combined))

; pkgs.writers.writeLua[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeLua(Bin)?$")
  (#set! injection.language "lua")
  (#set! injection.combined))

; pkgs.writers.writeNginxConfig name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeNginxConfig$")
  (#set! injection.language "nginx")
  (#set! injection.combined))

; pkgs.writers.writeGuile[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeGuile(Bin)?$")
  (#set! injection.language "scheme")
  (#set! injection.combined))

; pkgs.writers.writeBabashka[Bin] name attrs content
(apply_expression
  (apply_expression
    function: (apply_expression
      function: ((_) @_func)))
    argument: (indented_string_expression (string_fragment) @injection.content)
  (#match? @_func "(^|\\.)writeBabashka(Bin)?$")
  (#set! injection.language "clojure")
  (#set! injection.combined))

; Filename-based injection for indented strings.
;
; Detect the language from the file extension of a preceding filename
; argument in a curried call:
;
;   pkgs.writeText "index.html" ''
;     <div>Hello</div>
;   ''
;   pkgs.writeShellScriptBin "run.sh" ''
;     echo hi
;   ''
;
; The pattern matches `f "name.ext" '' ... ''` for any function `f`,
; minus a small denylist of common nixpkgs idioms that take a
; filename-shaped string but are not file writers (`removeSuffix`,
; `trace`, `throw`, etc.). Outside the denylist, false positives are
; tolerated: the worst case is mis-highlighting, whereas a false
; negative means no highlighting at all.
;
; Concept harvested from nix-community/tree-sitter-nix#169 by
; @nuketownada; rewritten as a hand-maintained list rather than
; generated from a Nix derivation, with the function denylist added
; per adversarial review of #53.
((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(sh|bash)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "bash")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(py)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "python")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(html|htm)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "html")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(css)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "css")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(js|mjs|cjs)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "javascript")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(ts|mts|cts)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "typescript")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(json)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "json")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(yml|yaml)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "yaml")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(toml)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "toml")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(lua)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "lua")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(nix)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "nix")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(xml)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "xml")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(md)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "markdown")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression
     function: (_) @_inner_func
     argument: (string_expression (string_fragment) @_filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_filename "\\.(sql)$")
 (#not-match? @_inner_func "(^|\\.)(removeSuffix|hasSuffix|hasPrefix|removePrefix|trace|throw|warn|warnIf|abort|assertMsg|seq|deepSeq|writeShellScript|writeShellScriptBin|runCommand|runCommandLocal|runCommandCC|runCommandNoCC)$")
 (#set! injection.language "sql")
 (#set! injection.combined))

((apply_expression
   function: (apply_expression function: (_) @_func
     argument: (string_expression (string_fragment) @injection.filename))
   argument: (indented_string_expression (string_fragment) @injection.content))
 (#match? @_func "(^|\\.)write(Text|Script(Bin)?)$")
 (#set! injection.combined))

; Let Helix infer a language from a shebang at the start of an indented string.
((indented_string_expression (string_fragment) @injection.shebang @injection.content)
 (#set! injection.combined))
)query";

    q(Odin) = R"query(
([(comment) (block_comment)] @injection.content
  (#set! injection.language "comment"))
)query";

    q(PHP) = R"query(
((text) @injection.content
 (#set! injection.language "html")
 (#set! injection.combined))

((comment) @injection.content
 (#set! injection.language "comment"))

((function_call_expression
 function: (name) @_function
 arguments: (arguments . (argument (_ (string_content) @injection.content))))
 (#match? @_function "^preg_")
 (#set! injection.language "regex"))

((function_call_expression
 function: (name) @_function
 arguments: (arguments (_) (argument (_ (string_content) @injection.content))))
 (#match? @_function "^mysqli_")
 (#set! injection.language "sql"))

((member_call_expression
 object: (_)
 name: (name) @_function
 arguments: (arguments . (argument (_ (string_content) @injection.content))))
 (#match? @_function "^(prepare|query)$")
 (#set! injection.language "sql"))
)query";

    q(PHP) = R"query(
((comment) @injection.content
  (#set! injection.language "comment"))

(heredoc
  (heredoc_body) @injection.content
  (heredoc_end) @injection.language)

(nowdoc
  (nowdoc_body) @injection.content
  (heredoc_end) @injection.language)
)query";

    q(Pascal) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(Perl) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
((pod) @injection.content
 (#set! injection.language "pod"))
)query";

    q(Prolog) = R"query(
((comment) @injection.content
  (#set! injection.language "comment"))
)query";

    q(Puppet) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(Python) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

; Match all 9 functions in the `re` module from the standard library that
; that takes a regex pattern as first argument.
; https://docs.python.org/3/library/re.html#functions
(call
  function: (attribute
    object: (identifier) @_module (#eq? @_module "re")
    attribute: (identifier) @_function (#any-of? @_function "compile" "search" "match" "fullmatch" "sub" "subn" "findall" "finditer" "split"))
  arguments: (argument_list
    . (string
        (string_content) @injection.content))
  (#set! injection.language "regex"))
)query";

    q(Ruby) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))

((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @name
  (#set! injection.language "sql"))
 (#eq? @name "SQL"))

((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @name
  (#set! injection.language "graphql"))
 (#any-of? @name
       "GQL"
       "GRAPHQL"))

((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @name
  (#set! injection.language "erb"))
 (#eq? @name "ERB"))

; `<command>`
; %x{<command>}
(subshell
  (string_content) @injection.content
  (#set! injection.language "bash"))

(call
  method: (identifier) @_method (#any-of? @_method "system" "spawn" "exec")
  arguments: (argument_list
    (string
      (string_content) @injection.content))
  (#set! injection.language "bash"))
)query";

    q(Rust) = R"query(
([(line_comment !doc) (block_comment !doc)] @injection.content
 (#set! injection.language "comment"))

((doc_comment) @injection.content
 (#set! injection.language "markdown-rustdoc")
 (#set! injection.combined))

((macro_invocation
  (token_tree) @injection.content)
 (#set! injection.language "rust")
 (#set! injection.include-children))

((macro_rule
  (token_tree) @injection.content)
 (#set! injection.language "rust")
 (#set! injection.include-children))

((macro_invocation
   macro:
     [
       (scoped_identifier
         name: (_) @_macro_name)
       (identifier) @_macro_name
     ]
   (token_tree) @injection.content)
 (#eq? @_macro_name "html")
 (#set! injection.language "html")
 (#set! injection.include-children))

((macro_invocation
   macro:
     [
       (scoped_identifier
         name: (_) @_macro_name)
       (identifier) @_macro_name
     ]
   (token_tree) @injection.content)
 (#eq? @_macro_name "slint")
 (#set! injection.language "slint")
 (#set! injection.include-children))

((macro_invocation
   macro:
     [
       (scoped_identifier name: (_) @_macro_name)
       (identifier) @_macro_name
     ]
   (token_tree
     (token_tree . "{" "}" .) @injection.content))
 (#eq? @_macro_name "json")
 (#set! injection.language "json")
 (#set! injection.include-children))

(call_expression
  function: (scoped_identifier
    path: (identifier) @_regex (#any-of? @_regex "Regex" "RegexBuilder")
    name: (identifier) @_new (#eq? @_new "new"))
  arguments:
    (arguments
      [
        (string_literal (string_content) @injection.content)
        (raw_string_literal (string_content) @injection.content)
      ])
  (#set! injection.language "regex"))

(call_expression
  function: (scoped_identifier
    path: (scoped_identifier (identifier) @_regex (#any-of? @_regex "Regex" "RegexBuilder") .)
    name: (identifier) @_new (#eq? @_new "new"))
  arguments:
    (arguments
      [
        (string_literal (string_content) @injection.content)
        (raw_string_literal (string_content) @injection.content)
      ])
  (#set! injection.language "regex"))

; Highlight SQL in `sqlx::query!()`, `sqlx::query_scalar!()`, and `sqlx::query_scalar_unchecked!()`
(macro_invocation
  macro: (scoped_identifier
    path: (identifier) @_sqlx (#eq? @_sqlx "sqlx")
    name: (identifier) @_query (#match? @_query "^query(_scalar|_scalar_unchecked)?$"))
  (token_tree
    ; Only the first argument is SQL
    .
    [
      (string_literal (string_content) @injection.content)
      (raw_string_literal (string_content) @injection.content)
    ]
  )
  (#set! injection.language "sql"))

; Highlight SQL in `sqlx::query_as!()` and `sqlx::query_as_unchecked!()`
(macro_invocation
  macro: (scoped_identifier
    path: (identifier) @_sqlx (#eq? @_sqlx "sqlx")
    name: (identifier) @_query_as (#match? @_query_as "^query_as(_unchecked)?$"))
  (token_tree
    ; Only the second argument is SQL
    .
    ; Allow anything as the first argument in case the user has lower case type
    ; names for some reason
    (_)
    [
      (string_literal (string_content) @injection.content)
      (raw_string_literal (string_content) @injection.content)
    ]
  )
  (#set! injection.language "sql"))

; Highlight SQL in `sqlx::query*` and `sqlx::raw_sql` functions
(call_expression
  function: (scoped_identifier
    path: (identifier) @_sqlx
    (#eq? @_sqlx "sqlx")
    name: (identifier) @_query_function)
  (#match? @_query_function "^query.*|raw_sql$")
  arguments: (arguments
    .
    [
      (string_literal
        (string_content) @injection.content)
      (raw_string_literal
        (string_content) @injection.content)
    ])
  (#set! injection.language "sql"))

; Special language `tree-sitter-rust-format-args` for Rust macros,
; which use `format_args!` under the hood and therefore have
; the `format_args!` syntax.
;
; This language is injected into a hard-coded set of macros.
(
  (macro_invocation
    macro:
      [
        (scoped_identifier
          name: (_) @_macro_name)
        (identifier) @_macro_name
      ]
    (token_tree) @injection.content
  )
  (#any-of? @_macro_name
    ; 1st argument is `format_args!`

    ; std
    "print" "println" "eprint" "eprintln"
    "format" "format_args" "todo" "panic"
    "unreachable" "unimplemented" "compile_error"
    ; log
    "crit" "trace" "debug" "info" "warn" "error"
    ; anyhow
    "anyhow" "bail"
    ; syn
    "format_ident"
    ; indoc
    "formatdoc" "printdoc" "eprintdoc" "writedoc"
    ; iced
    "text"
    ; ratatui
    "span"
    ; eyre
    "eyre"
    ; miette
    "miette"

    ; 2nd argument is `format_args!`

    ; std
    "write" "writeln" "assert" "debug_assert"
    ; defmt
    "expect" "unwrap"
    ; ratatui
    "span"

    ; 3rd argument is `format_args!`

    ; std
    "assert_eq" "debug_assert_eq" "assert_ne" "debug_assert_ne"

    ; Dioxus's rsx! macro accepts string interpolation in all
    ; strings, across the entire token tree
    "rsx"
  )
  (#set! injection.language "rust-format-args-macro")
  (#set! injection.include-children)
)

; for some queries (e.g. when you have generic table names) you need to wrap it in `AssertSqlSafe`
; after `format!` so it can overwrite `format!` formatting correctly.
(call_expression
  function: [
    (scoped_identifier
      path: (identifier) @_sqlx
      (#eq? @_sqlx "sqlx")
      name: (identifier) @_AssertSqlSafe)
    (identifier) @_AssertSqlSafe
  ]
  (#eq? @_AssertSqlSafe "AssertSqlSafe")
  arguments: (arguments
    [
      (string_literal
        (string_content) @injection.content)
      (raw_string_literal
        (string_content) @injection.content)
      (macro_invocation
        macro: (identifier) @_format
        (#eq? @_format "format")
        (token_tree
          [
            (string_literal
              (string_content) @injection.content)
            (raw_string_literal
              (string_content) @injection.content)
          ]))
    ])
  (#set! injection.language "sql"))
)query";

    // HACK: This language is the same as Rust but all strings are injected
    // with rust-format-args. Rust injects this into known macros which use
    // the format args syntax. This can cause false-positives but
    // those are expected to be rare.
    q(RustFormatArgsMacro) = q(Rust) + R"query(
([
   (string_literal (string_content) @injection.content)
   (raw_string_literal (string_content) @injection.content)
 ]
 (#set! injection.language "rust-format-args")
 (#set! injection.include-children))
)query";

    q(Scala) = R"query(
([(comment) (block_comment)] @injection.content
 (#set! injection.language "comment"))

; Matches these SQL interpolators:
;  - Doobie: 'sql', 'fr'
;  - Quill: 'sql', 'infix'
;  - Slick: 'sql', 'sqlu'
(interpolated_string_expression
  interpolator:
    ((identifier) @interpolator
     (#any-of? @interpolator "fr" "infix" "sql" "sqlu"))
  (interpolated_string) @injection.content
  (#set! injection.language "sql"))
)query";

    q(Scheme) = R"query(
([(comment) (block_comment)] @injection.content
 (#set! injection.language "comment"))
)query";

    q(Slint) = R"query(
([(line_comment) (block_comment)] @injection.content
 (#set! injection.language "comment"))
)query";

    q(Swift) = R"query(
((regex_literal) @injection.content
 (#set! injection.language "regex"))

((comment) @injection.content
 (#set! injection.language "comment")
 (#set! injection.include-children))
)query";

    q(TOML) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(Verilog) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(YAML) = R"query(
((comment) @injection.content
 (#set! injection.language "comment"))
)query";

    q(Zig) = R"query(
((comment) @injection.content
  (#set! injection.language "comment"))
)query";

    return queries;
}();

#define DEFLANG(L) \
    L(ADL, "adl", adl)\
    L(Ada, "ada", ada)\
    L(Agda, "agda", agda)\
    L(Alloy, "alloy", alloy)\
    L(Amber, "amber", amber)\
    L(Astro, "astro", astro)\
    L(Awk, "awk", awk)\
    L(Bash, "bash", bash)\
    L(Basic, "basic", basic)\
    L(Bass, "bass", bass)\
    L(Batch, "batch", batch)\
    L(Beancount, "beancount", beancount)\
    L(BibTeX, "bibtex", bibtex)\
    L(Bicep, "bicep", bicep)\
    L(BitBake, "bitbake", bitbake)\
    L(Blade, "blade", blade)\
    L(Blueprint, "blueprint", blueprint)\
    L(C, "c", c)\
    L(C3, "c3", c3)\
    L(CEL, "cel", cel)\
    L(CMake, "cmake", cmake)\
    L(CPON, "cpon", cpon)\
    L(CPlusPlus, "cpp", cpp)\
    L(CSS, "css", css)\
    L(CSV, "csv", csv)\
    L(CSharp, "c-sharp", c_sharp)\
    L(Caddyfile, "caddyfile", caddyfile)\
    L(Cairo, "cairo", cairo)\
    L(CapNProto, "capnp", capnp)\
    L(ChucK, "chuck", chuck)\
    L(Circom, "circom", circom)\
    L(Clarity, "clarity", clarity)\
    L(Clojure, "clojure", clojure)\
    L(Comment, "comment", comment)\
    L(CommonLisp, "common-lisp", commonlisp)\
    L(Crystal, "crystal", crystal)\
    L(Cue, "cue", cue)\
    L(Cylc, "cylc", cylc)\
    L(Cython, "cython", cython)\
    L(D, "d", d)\
    L(DBML, "dbml", dbml)\
    L(DTD, "dtd", dtd)\
    L(Dart, "dart", dart)\
    L(Debian, "debian", debian)\
    L(DeviceTree, "devicetree", devicetree)\
    L(Dhall, "dhall", dhall)\
    L(Diff, "diff", diff)\
    L(Djot, "djot", djot)\
    L(Dockerfile, "dockerfile", dockerfile)\
    L(Dot, "dot", dot)\
    L(Doxyfile, "doxyfile", doxyfile)\
    L(Doxygen, "doxygen", doxygen)\
    L(Drools, "drools", drools)\
    L(Dunstrc, "dunstrc", dunstrc)\
    L(EBNF, "ebnf", ebnf)\
    L(EEX, "eex", eex)\
    L(Earthfile, "earthfile", earthfile)\
    L(Eiffel, "eiffel", eiffel)\
    L(Elixir, "elixir", elixir)\
    L(Elm, "elm", elm)\
    L(Elvish, "elvish", elvish)\
    L(EmacsLisp, "elisp", elisp)\
    L(EmbeddedTemplate, "embedded-template", embedded_template)\
    L(Erlang, "erlang", erlang)\
    L(FGA, "fga", fga)\
    L(FIDL, "fidl", fidl)\
    L(FSharp, "fsharp", fsharp)\
    L(Fennel, "fennel", fennel)\
    L(Fish, "fish", fish)\
    L(FlatBuffers, "flatbuffers", flatbuffers)\
    L(Forth, "forth", forth)\
    L(Fortran, "fortran", fortran)\
    L(FreeBasic, "freebasic", freebasic)\
    L(GAS, "gas", gas)\
    L(GDScript, "gdscript", gdscript)\
    L(GLSL, "glsl", glsl)\
    L(GN, "gn", gn)\
    L(GPR, "gpr", gpr)\
    L(Gemini, "gemini", gemini)\
    L(Gherkin, "gherkin", gherkin)\
    L(Ghostty, "Ghostty", ghostty)\
    L(GitAttributes, "git-attributes", gitattributes)\
    L(GitCommit, "git-commit", gitcommit)\
    L(GitConfig, "git-config", git_config)\
    L(GitIgnore, "git-ignore", gitignore)\
    L(GitRebase, "git-rebase", git_rebase)\
    L(Gleam, "gleam", gleam)\
    L(Gnuplot, "gnuplot", gnuplot)\
    L(Go, "go", go)\
    L(GoFormatString, "go-format-string", go_format_string)\
    L(GodotResource, "godot", godot_resource)\
    L(GraphQL, "graphql", graphql)\
    L(Gren, "gren", gren)\
    L(Groovy, "groovy", groovy)\
    L(HCL, "hcl", hcl)\
    L(HDL, "hdl", hdl)\
    L(HEEX, "heex", heex)\
    L(HOCON, "hocon", hocon)\
    L(HTML, "html", html)\
    L(HTMLDjango, "html-django", htmldjango)\
    L(Hare, "hare", hare)\
    L(Haskell, "haskell", haskell)\
    L(HaskellLiterate, "haskell-literate", haskell_literate)\
    L(HaskellPersistent, "haskell-persistent", haskell_persistent)\
    L(Haxe, "haxe", haxe)\
    L(Hoon, "hoon", hoon)\
    L(Hosts, "hosts", hosts)\
    L(Hurl, "hurl", hurl)\
    L(Hyprlang, "hyprlang", hyprlang)\
    L(IEX, "iex", iex)\
    L(INI, "ini", ini)\
    L(Ink, "ink", ink)\
    L(Inko, "inko", inko)\
    L(JJDescription, "jjdescription", jjdescription)\
    L(JJRevset, "jjrevset", jjrevset)\
    L(JJTemplate, "jjtemplate", jjtemplate)\
    L(JQ, "jq", jq)\
    L(JSDoc, "jsdoc", jsdoc)\
    L(JSON, "json", json)\
    L(JSON5, "json5", json5)\
    L(JanetSimple, "janet", janet_simple)\
    L(Java, "java", java)\
    L(JavaScript, "javascript", javascript)\
    L(Jinja2, "jinja2", jinja2)\
    L(Jsonnet, "jsonnet", jsonnet)\
    L(Julia, "julia", julia)\
    L(Just, "just", just)\
    L(KCL, "kcl", kcl)\
    L(KConfig, "kconfig", kconfig)\
    L(KDL, "kdl", kdl)\
    L(Klog, "klog", klog)\
    L(Koka, "koka", koka)\
    L(Kotlin, "kotlin", kotlin)\
    L(Koto, "koto", koto)\
    L(LD, "ld", ld)\
    L(LDIF, "ldif", ldif)\
    L(LPF, "lpf", lpf)\
    L(LaTeX, "latex", latex)\
    L(Lean, "lean", lean)\
    L(Ledger, "ledger", ledger)\
    L(Less, "less", less)\
    L(Log, "log", log)\
    L(Lua, "lua", lua)\
    L(LuaFormatString, "lua-format-string", lua_format_string)\
    L(LuaPattern, "luap", luap)\
    L(Luau, "luau", luau)\
    L(MATLAB, "matlab", matlab)\
    L(Mail, "mail", mail)\
    L(Make, "make", make)\
    L(Markdoc, "markdoc", markdoc)\
    L(Markdown, "markdown", markdown)\
    L(MarkdownInline, "markdown.inline", markdown_inline)\
    L(MarkdownRustdoc, "markdown-rustdoc", markdown)\
    L(Mermaid, "mermaid", mermaid)\
    L(Meson, "meson", meson)\
    L(Mojo, "mojo", mojo)\
    L(MoonBit, "moonbit", moonbit)\
    L(Move, "move", move)\
    L(NASM, "nasm", nasm)\
    L(Nearley, "nearley", nearley)\
    L(Nginx, "nginx", nginx)\
    L(Nickel, "nickel", nickel)\
    L(Nim, "nim", nim)\
    L(Nix, "nix", nix)\
    L(Nu, "nu", nu)\
    L(OHM, "ohm", ohm)\
    L(Odin, "odin", odin)\
    L(OpenCL, "opencl", opencl)\
    L(OpenSCAD, "openscad", openscad)\
    L(Org, "org", org)\
    L(P, "p", p)\
    L(PEM, "pem", pem)\
    L(PHP, "php", php)\
    L(PHPOnly, "php-only", php_only)\
    L(PKL, "pkl", pkl)\
    L(PO, "po", po)\
    L(PRQL, "prql", prql)\
    L(PTX, "ptx", ptx)\
    L(Pascal, "pascal", pascal)\
    L(Passwd, "passwd", passwd)\
    L(Penrose, "penrose", penrose)\
    L(Perl, "perl", perl)\
    L(Picat, "picat", picat)\
    L(PlainOldDocumentation, "pod", pod)\
    L(PonyLang, "ponylang", ponylang)\
    L(PowerShell, "powershell", powershell)\
    L(Prisma, "prisma", prisma)\
    L(ProVerif, "proverif", proverif)\
    L(Prolog, "prolog", prolog)\
    L(Properties, "properties", properties)\
    L(Proto, "proto", proto)\
    L(Pug, "pug", pug)\
    L(Puppet, "puppet", puppet)\
    L(PureScript, "purescript", purescript)\
    L(Python, "python", python)\
    L(QL, "ql", ql)\
    L(QMLJS, "qmljs", qmljs)\
    L(R, "r", r)\
    L(RON, "ron", ron)\
    L(RPMBash, "rpmbash", rpmbash)\
    L(RPMSpec, "rpmspec", rpmspec)\
    L(RSHTML, "rshtml", rshtml)\
    L(ReScript, "rescript", rescript)\
    L(ReStructuredText, "rst", rst)\
    L(Rego, "rego", rego)\
    L(RegularExpression, "regex", regex)\
    L(Requirements, "requirements", requirements)\
    L(Robot, "robot", robot)\
    L(RobotsTxt, "robots.txt", robots_txt)\
    L(Ruby, "ruby", ruby)\
    L(Rust, "rust", rust)\
    L(RustFormatArgs, "rust-format-args", rust_format_args)\
    L(RustFormatArgsMacro, "rust-format-args-macro", rust)\
    L(SCFG, "scfg", scfg)\
    L(SCSS, "scss", scss)\
    L(SlightLisp, "slisp", slisp)\
    L(SML, "sml", sml)\
    L(SQL, "sql", sql)\
    L(SSHClientConfig, "ssh-client-config", ssh_client_config)\
    L(Scala, "scala", scala)\
    L(Scheme, "scheme", scheme)\
    L(ShellCheckRC, "shellcheckrc", shellcheckrc)\
    L(Slang, "slang", slang)\
    L(Slint, "slint", slint)\
    L(Smali, "smali", smali)\
    L(Smithy, "smithy", smithy)\
    L(Snakemake, "snakemake", snakemake)\
    L(Solidity, "solidity", solidity)\
    L(SourcePawn, "sourcepawn", sourcepawn)\
    L(Spade, "spade", spade)\
    L(SpiceDB, "spicedb", spicedb)\
    L(Strace, "strace", strace)\
    L(StrictDoc, "strictdoc", strictdoc)\
    L(SuperCollider, "supercollider", supercollider)\
    L(Sway, "sway", sway)\
    L(Swift, "swift", swift)\
    L(SystemVerilog, "systemverilog", systemverilog)\
    L(T32, "t32", t32)\
    L(TCL, "tcl", tcl)\
    L(TLAPlus, "tlaplus", tlaplus)\
    L(TOML, "toml", toml)\
    L(TQL, "tql", tql)\
    L(TSX, "tsx", tsx)\
    L(TableGen, "tablegen", tablegen)\
    L(Tact, "tact", tact)\
    L(Task, "task", task)\
    L(Teal, "teal", teal)\
    L(Templ, "templ", templ)\
    L(Tera, "tera", tera)\
    L(TextProto, "textproto", textproto)\
    L(Thrift, "thrift", thrift)\
    L(TodoTxt, "TODO.txt", todotxt)\
    L(TreeSitterQuery, "query", query)\
    L(Twig, "twig", twig)\
    L(TypeScript, "typescript", typescript)\
    L(TypeSpec, "typespec", typespec)\
    L(Typst, "typst", typst)\
    L(Ungrammar, "ungrammar", ungrammar)\
    L(Unison, "unison", unison)\
    L(Uxntal, "uxntal", uxntal)\
    L(VHDL, "vhdl", vhdl)\
    L(VHS, "vhs", vhs)\
    L(Vala, "vala", vala)\
    L(Vento, "vento", vento)\
    L(Verilog, "verilog", verilog)\
    L(Vim, "vim", vim)\
    L(WESL, "wesl", wesl)\
    L(WGSL, "wgsl", wgsl)\
    L(Werk, "werk", werk)\
    L(WikiText, "wikitext", wikitext)\
    L(Wren, "wren", wren)\
    L(XML, "xml", xml)\
    L(XTC, "xtc", xtc)\
    L(Xit, "xit", xit)\
    L(YAML, "yaml", yaml)\
    L(YARA, "yara", yara)\
    L(Yuck, "yuck", yuck)\
    L(Zig, "zig", zig)

#define TS_LANG(Language, Name, Grammar) \
    extern "C" const TSLanguage *tree_sitter_##Grammar();

DEFLANG(TS_LANG)

#undef TS_LANG

constexpr LanguageInfo languages[] = {
#define LANG(EnumName, Name, Grammar) \
    { Syntax::Language::EnumName, Name, tree_sitter_##Grammar },
    DEFLANG(LANG)
#undef LANG
};

constexpr const LanguageInfo& language_info(Syntax::Language language) noexcept
{
    return *std::ranges::find(languages, language, &LanguageInfo::language);
}

struct LanguageAlias
{
    std::string_view name;
    Syntax::Language language;
};

constexpr const LanguageAlias aliases[] = {
    { "erb", Syntax::Language::EmbeddedTemplate },
    { "md", Syntax::Language::Markdown },
    { "sh", Syntax::Language::Bash }
};

struct FilenameLanguages
{
    Syntax::Language language;
    std::string_view names;
};

// Space-separated lists keep the filename catalogue compact while retaining
// exact, case-sensitive matching. Entries are based on the standalone file
// types in Helix's language catalogue; fragment-only grammars are omitted.
constexpr FilenameLanguages suffix_languages[] = {
    { Syntax::Language::ADL, "adl" },
    { Syntax::Language::Ada, "adb ads" },
    { Syntax::Language::Agda, "agda" },
    { Syntax::Language::Alloy, "alloy" },
    { Syntax::Language::Amber, "ab" },
    { Syntax::Language::Astro, "astro" },
    { Syntax::Language::Awk, "awk gawk mawk nawk" },
    { Syntax::Language::Bash, "Renviron ash bash bashrc_Apple_Terminal bazelrc cshrc dash ebuild eclass ksh mksh sh tcshrc zlogin zlogout zprofile zsh zsh-theme zshenv zshrc zshrc_Apple_Terminal" },
    { Syntax::Language::Basic, "bas" },
    { Syntax::Language::Bass, "bass" },
    { Syntax::Language::Batch, "bat btm cmd" },
    { Syntax::Language::Beancount, "bean beancount" },
    { Syntax::Language::BibTeX, "bib" },
    { Syntax::Language::Bicep, "bicep bicepparam" },
    { Syntax::Language::BitBake, "bb bbappend bbclass" },
    { Syntax::Language::Blade, "blade blade.php" },
    { Syntax::Language::Blueprint, "blp" },
    { Syntax::Language::C, "c" },
    { Syntax::Language::C3, "c3 c3i c3t" },
    { Syntax::Language::CEL, "cel" },
    { Syntax::Language::CMake, "cmake" },
    { Syntax::Language::CPON, "cp cpon" },
    { Syntax::Language::CPlusPlus, "C H c++ cc cpp cppm cu cuh cxx h h++ hh hpp hxx ii inl ino ipp ixx tpp txx" },
    { Syntax::Language::CSS, "css" },
    { Syntax::Language::CSharp, "cake cs csx" },
    { Syntax::Language::Caddyfile, "Caddyfile caddyfile" },
    { Syntax::Language::Cairo, "cairo" },
    { Syntax::Language::CapNProto, "capnp" },
    { Syntax::Language::ChucK, "ck" },
    { Syntax::Language::Circom, "circom" },
    { Syntax::Language::Clarity, "clar" },
    { Syntax::Language::Clojure, "boot clj cljc clje cljr cljs cljx edn" },
    { Syntax::Language::CommonLisp, "asd l lisp lsp ny podsl ros sexp" },
    { Syntax::Language::Crystal, "cr" },
    { Syntax::Language::Cue, "cue" },
    { Syntax::Language::Cylc, "cylc" },
    { Syntax::Language::Cython, "pxd pxi pyx" },
    { Syntax::Language::D, "d dd" },
    { Syntax::Language::DBML, "dbml" },
    { Syntax::Language::DTD, "dtd ent" },
    { Syntax::Language::Dart, "dart" },
    { Syntax::Language::Debian, "changes dsc" },
    { Syntax::Language::DeviceTree, "dts dtsi" },
    { Syntax::Language::Dhall, "dhall" },
    { Syntax::Language::Diff, "diff patch rej" },
    { Syntax::Language::Djot, "dj djot" },
    { Syntax::Language::Dockerfile, "Containerfile Dockerfile containerfile dockerfile" },
    { Syntax::Language::Dot, "dot" },
    { Syntax::Language::Drools, "drl" },
    { Syntax::Language::EBNF, "ebnf" },
    { Syntax::Language::EEX, "eex" },
    { Syntax::Language::Eiffel, "e" },
    { Syntax::Language::Elixir, "ex exs" },
    { Syntax::Language::Elm, "elm" },
    { Syntax::Language::Elvish, "elv" },
    { Syntax::Language::EmacsLisp, "el" },
    { Syntax::Language::EmbeddedTemplate, "erb" },
    { Syntax::Language::Erlang, "app app.src erl hrl" },
    { Syntax::Language::FGA, "fga" },
    { Syntax::Language::FIDL, "fidl" },
    { Syntax::Language::FSharp, "fs fsx fsi fsscript" },
    { Syntax::Language::Fennel, "fnl fnlm" },
    { Syntax::Language::Fish, "fish" },
    { Syntax::Language::FlatBuffers, "fbs" },
    { Syntax::Language::Forth, "4th forth fs fth" },
    { Syntax::Language::Fortran, "F F03 F90 F95 f f03 f90 f95 for" },
    { Syntax::Language::FreeBasic, "bi" },
    { Syntax::Language::GAS, "s" },
    { Syntax::Language::GDScript, "gd" },
    { Syntax::Language::GLSL, "comp frag geom glsl tesc tese vert" },
    { Syntax::Language::GN, "gn gni" },
    { Syntax::Language::GPR, "gpr" },
    { Syntax::Language::Gemini, "gmi" },
    { Syntax::Language::Gherkin, "feature" },
    { Syntax::Language::Ghostty, "ghostty" },
    { Syntax::Language::GitConfig, "gitconfig" },
    { Syntax::Language::Gleam, "gleam" },
    { Syntax::Language::Gnuplot, "gnuplot plot plt" },
    { Syntax::Language::Go, "go" },
    { Syntax::Language::GodotResource, "gdextension godot tres tscn" },
    { Syntax::Language::GraphQL, "gql graphql graphqls" },
    { Syntax::Language::Gren, "gren" },
    { Syntax::Language::Groovy, "gradle groovy jenkinsfile" },
    { Syntax::Language::HCL, "hcl nomad tf" },
    { Syntax::Language::HDL, "hdl" },
    { Syntax::Language::HEEX, "heex" },
    { Syntax::Language::HTML, "asp aspx cshtml htm html jshtm jsp rhtml shtml volt xht xhtml" },
    { Syntax::Language::Hare, "ha" },
    { Syntax::Language::Haskell, "hs hs-boot hsc" },
    { Syntax::Language::HaskellLiterate, "lhs" },
    { Syntax::Language::HaskellPersistent, "persistentmodels" },
    { Syntax::Language::Haxe, "hx" },
    { Syntax::Language::Hoon, "hoon" },
    { Syntax::Language::Hurl, "hurl" },
    { Syntax::Language::IEX, "iex" },
    { Syntax::Language::INI, "cfg desktop directory ini" },
    { Syntax::Language::Ink, "ink" },
    { Syntax::Language::Inko, "inko" },
    { Syntax::Language::JJDescription, "jjdescription" },
    { Syntax::Language::JJRevset, "jjrevset" },
    { Syntax::Language::JJTemplate, "jjtemplate" },
    { Syntax::Language::JQ, "jq" },
    { Syntax::Language::JSDoc, "jsdoc" },
    { Syntax::Language::JSON, "arb avsc code-workspace css.map geojson gltf ipynb js.map json jsonl ldtk ldtkl sublime-build sublime-color-scheme sublime-commands sublime-completions sublime-keymap sublime-macro sublime-menu sublime-mousemap sublime-project sublime-settings sublime-theme sublime-workspace tfstate tfstate.backup ts.map webmanifest" },
    { Syntax::Language::JSON5, "json5" },
    { Syntax::Language::JanetSimple, "cgen janet jdn" },
    { Syntax::Language::Java, "jav java pde" },
    { Syntax::Language::JavaScript, "cjs es6 gs js mjs pac rules" },
    { Syntax::Language::Jinja2, "j2 jinja jinja2 njk" },
    { Syntax::Language::Jsonnet, "jsonnet libsonnet" },
    { Syntax::Language::Julia, "jl" },
    { Syntax::Language::Just, "just" },
    { Syntax::Language::KCL, "kcl" },
    { Syntax::Language::KConfig, "Kconfig" },
    { Syntax::Language::KDL, "kdl" },
    { Syntax::Language::Klog, "klg" },
    { Syntax::Language::Koka, "kk" },
    { Syntax::Language::Kotlin, "kt kts" },
    { Syntax::Language::Koto, "koto" },
    { Syntax::Language::LD, "ld" },
    { Syntax::Language::LDIF, "ldif" },
    { Syntax::Language::LPF, "lpf" },
    { Syntax::Language::LaTeX, "Rd bbx cbx cls dtx ins sty tex" },
    { Syntax::Language::Lean, "lean" },
    { Syntax::Language::Ledger, "journal ldg ledger" },
    { Syntax::Language::Less, "less" },
    { Syntax::Language::Log, "log" },
    { Syntax::Language::Lua, "lua rockspec" },
    { Syntax::Language::Luau, "luau" },
    { Syntax::Language::MATLAB, "m" },
    { Syntax::Language::Mail, "eml" },
    { Syntax::Language::Make, "mak make mk" },
    { Syntax::Language::Markdoc, "mdoc" },
    { Syntax::Language::Markdown, "livemd markdn markdown md mdown mdtext mdtxt mdwn mdx mkd mkdn workbook" },
    { Syntax::Language::Mermaid, "mermaid mmd" },
    { Syntax::Language::Mojo, "mojo 🔥" },
    { Syntax::Language::MoonBit, "mbt mbti" },
    { Syntax::Language::Move, "move" },
    { Syntax::Language::NASM, "S asm nasm" },
    { Syntax::Language::Nearley, "ne" },
    { Syntax::Language::Nickel, "ncl" },
    { Syntax::Language::Nim, "nim nimble nims" },
    { Syntax::Language::Nix, "nix" },
    { Syntax::Language::Nu, "nu nuon" },
    { Syntax::Language::OHM, "ohm" },
    { Syntax::Language::Odin, "odin" },
    { Syntax::Language::OpenCL, "cl" },
    { Syntax::Language::OpenSCAD, "scad" },
    { Syntax::Language::Org, "org" },
    { Syntax::Language::PEM, "cert crt pem" },
    { Syntax::Language::PHP, "php php4 php5 phtml ctp" },
    { Syntax::Language::PKL, "pcf pkl" },
    { Syntax::Language::PO, "po pot" },
    { Syntax::Language::PRQL, "prql" },
    { Syntax::Language::PTX, "ptx" },
    { Syntax::Language::Pascal, "inc lfm lpr pas" },
    { Syntax::Language::Penrose, "domain style substance" },
    { Syntax::Language::Perl, "nqp p6 pl pl6 pm pm6 psgi raku rakudoc rakumod rakutest t" },
    { Syntax::Language::Picat, "pi picat" },
    { Syntax::Language::PlainOldDocumentation, "pod" },
    { Syntax::Language::PonyLang, "pony" },
    { Syntax::Language::PowerShell, "ps1 pscc psd1 psm1 psrc" },
    { Syntax::Language::Prisma, "prisma" },
    { Syntax::Language::ProVerif, "pv" },
    { Syntax::Language::Properties, "prefs properties" },
    { Syntax::Language::Proto, "proto" },
    { Syntax::Language::Pug, "pug" },
    { Syntax::Language::PureScript, "purs" },
    { Syntax::Language::Python, "cpy ipy ptl py py3 pyi pyt pyw rpy" },
    { Syntax::Language::QL, "ql qll" },
    { Syntax::Language::QMLJS, "qml" },
    { Syntax::Language::R, "R r" },
    { Syntax::Language::RON, "ron" },
    { Syntax::Language::RSHTML, "rs.html" },
    { Syntax::Language::ReScript, "res" },
    { Syntax::Language::ReStructuredText, "rst" },
    { Syntax::Language::Rego, "rego" },
    { Syntax::Language::RegularExpression, "regex" },
    { Syntax::Language::Robot, "resource robot" },
    { Syntax::Language::Ruby, "Brewfile gemspec irb jb jbuilder podspec rabl rake rb rbi rbs rjs" },
    { Syntax::Language::Rust, "rs" },
    { Syntax::Language::SCSS, "scss" },
    { Syntax::Language::SlightLisp, "sl" },
    { Syntax::Language::SML, "fun sig sml" },
    { Syntax::Language::SQL, "dsql sql" },
    { Syntax::Language::Scala, "sbt scala" },
    { Syntax::Language::Scheme, "scm sld ss" },
    { Syntax::Language::Slang, "slang" },
    { Syntax::Language::Slint, "slint" },
    { Syntax::Language::Smali, "smali" },
    { Syntax::Language::Smithy, "smithy" },
    { Syntax::Language::Snakemake, "smk" },
    { Syntax::Language::Solidity, "sol" },
    { Syntax::Language::SourcePawn, "sp" },
    { Syntax::Language::Spade, "spade" },
    { Syntax::Language::SpiceDB, "zed" },
    { Syntax::Language::Strace, "strace" },
    { Syntax::Language::StrictDoc, "sdoc sgra" },
    { Syntax::Language::SuperCollider, "quark sc scd" },
    { Syntax::Language::Sway, "sw" },
    { Syntax::Language::Swift, "swift swiftinterface" },
    { Syntax::Language::SystemVerilog, "sv svh" },
    { Syntax::Language::T32, "cmm t32" },
    { Syntax::Language::TCL, "tcl" },
    { Syntax::Language::TLAPlus, "tla" },
    { Syntax::Language::TOML, "toml" },
    { Syntax::Language::TQL, "tql" },
    { Syntax::Language::TSX, "tsx" },
    { Syntax::Language::TableGen, "td" },
    { Syntax::Language::Tact, "tact" },
    { Syntax::Language::Task, "task" },
    { Syntax::Language::Teal, "tl" },
    { Syntax::Language::Templ, "templ" },
    { Syntax::Language::Tera, "tera" },
    { Syntax::Language::TextProto, "textpb textproto txtpb" },
    { Syntax::Language::Thrift, "thrift" },
    { Syntax::Language::TodoTxt, "todo.txt todotxt" },
    { Syntax::Language::Twig, "twig" },
    { Syntax::Language::TypeScript, "cts mts ts" },
    { Syntax::Language::TypeSpec, "tsp" },
    { Syntax::Language::Typst, "typ typst" },
    { Syntax::Language::Ungrammar, "ungram ungrammar" },
    { Syntax::Language::Unison, "u" },
    { Syntax::Language::Uxntal, "tal" },
    { Syntax::Language::VHDL, "vhd vhdl" },
    { Syntax::Language::VHS, "tape" },
    { Syntax::Language::Vala, "vala vapi" },
    { Syntax::Language::Vento, "vto" },
    { Syntax::Language::Verilog, "v vh" },
    { Syntax::Language::Vim, "vim" },
    { Syntax::Language::WESL, "wesl" },
    { Syntax::Language::WGSL, "wgsl" },
    { Syntax::Language::Werk, "werk" },
    { Syntax::Language::WikiText, "mediawiki wikimedia wikitext" },
    { Syntax::Language::Wren, "wren" },
    { Syntax::Language::XML, "ascx atom axaml axml bpmn checkstyle cpt csl csproj.user dita ditamap dtml fods fodt fxml gir glif gml gpx iml isml itermcolors jmx kml launch menu mobileconfig mpd musicxml mxml ncx nuspec opml osc osm plist policy pproj pt publishsettings pubxml pubxml.user rbxlx rbxmx resx rng rss shproj smil storyboard sublime-snippet svg terminal tld tmx ui vbproj.user vcxproj vcxproj.filters wixproj wsdl wxi wxs xaml xbl xib xlf xliff xml xmp xoml xpdl xrc xsd xsl xul" },
    { Syntax::Language::XTC, "xoa xpc xtc" },
    { Syntax::Language::Xit, "xit" },
    { Syntax::Language::YAML, "bu sublime-syntax yaml yml" },
    { Syntax::Language::YARA, "yar yara" },
    { Syntax::Language::Yuck, "yuck" },
    { Syntax::Language::Zig, "zig zon" },
};

constexpr FilenameLanguages basename_languages[] = {
    { Syntax::Language::Bash, ".Renviron .bash_aliases .bash_history .bash_login .bash_logout .bash_profile .bashrc .hushlogin .profile .sh_history .tmux.conf .xinitrc .xprofile .xserverrc .xsession .xsessionrc .yash_profile .yashrc .zimrc .zlogin .zlogout .zprofile .zshenv .zshrc APKBUILD bash_completion direnvrc tmux.conf xinitrc xserverrc" },
    { Syntax::Language::CMake, "CMakeLists.txt" },
    { Syntax::Language::CPlusPlus, ".h.in .hpp.in" },
    { Syntax::Language::Caddyfile, "Caddyfile caddyfile" },
    { Syntax::Language::Cylc, "suite.rc" },
    { Syntax::Language::Dockerfile, "Containerfile Dockerfile containerfile dockerfile" },
    { Syntax::Language::Doxyfile, "Doxyfile" },
    { Syntax::Language::Dunstrc, "dunstrc" },
    { Syntax::Language::Earthfile, "Earthfile" },
    { Syntax::Language::Elixir, "mix.lock" },
    { Syntax::Language::Erlang, "rebar.config rebar.lock" },
    { Syntax::Language::FGA, "fga.mod" },
    { Syntax::Language::GitAttributes, ".gitattributes" },
    { Syntax::Language::GitCommit, "COMMIT_EDITMSG MERGE_MSG TAG_EDITMSG" },
    { Syntax::Language::GitConfig, ".gitconfig .gitmodules config.worktree gitconfig" },
    { Syntax::Language::GitIgnore, ".dockerignore .git-blame-ignore-revs .gitignore .gitignore_global .ignore .npmignore CODEOWNERS" },
    { Syntax::Language::GitRebase, "git-rebase-todo" },
    { Syntax::Language::Groovy, "Jenkinsfile" },
    { Syntax::Language::Hosts, "hosts" },
    { Syntax::Language::INI, ".buckconfig .buckconfig.local .editorconfig .npmrc .wslconfig hgrc mimeapps.list npmrc rclone.conf" },
    { Syntax::Language::JSON, ".babelrc .bowerrc .jscrc .jslintrc .swift-format .vuerc .watchmanconfig composer.lock deno.lock devbox.lock devenv.lock flake.lock manifest.lock" },
    { Syntax::Language::JavaScript, ".node_repl_history jakefile" },
    { Syntax::Language::Just, ".Justfile .justfile Justfile justfile" },
    { Syntax::Language::KConfig, "Kconfig" },
    { Syntax::Language::Make, "GNUmakefile Makefile OCamlMakefile makefile" },
    { Syntax::Language::Markdown, "PULLREQ_EDITMSG" },
    { Syntax::Language::Meson, "meson.build meson.options meson_options.txt" },
    { Syntax::Language::Nginx, "nginx.conf" },
    { Syntax::Language::PKL, "PklProject" },
    { Syntax::Language::Passwd, "passwd" },
    { Syntax::Language::Perl, ".latexmkrc latexmkrc" },
    { Syntax::Language::Python, ".python_history .pythonrc .pythonstartup SConscript SConstruct" },
    { Syntax::Language::R, ".RHistory .Rprofile .radian_profile Rprofile.site" },
    { Syntax::Language::RegularExpression, ".Rbuildignore" },
    { Syntax::Language::Requirements, "constraints.txt requirements.txt" },
    { Syntax::Language::RobotsTxt, "robots.txt" },
    { Syntax::Language::Ruby, ".irbrc .simplecov Appfile Appraisals Berksfile Berksfile.lock Brewfile Capfile Cheffile Deliverfile Fastfile Gemfile Guardfile Gymfile Hobofile Matchfile Podfile Puppetfile Rakefile Rantfile Scanfile Snapfile Thorfile Vagrantfile gemfile rakefile" },
    { Syntax::Language::ShellCheckRC, ".shellcheckrc shellcheckrc" },
    { Syntax::Language::Snakemake, "Snakefile" },
    { Syntax::Language::TOML, "Cargo.lock containers.conf mounts.conf pdm.lock poetry.lock policy.conf registries.conf staticcheck.conf storage.conf uv.lock" },
    { Syntax::Language::TreeSitterQuery, "highlights.scm indents.scm injections.scm locals.scm tags.scm textobjects.scm" },
    { Syntax::Language::Vim, ".exrc .nvimrc .vimrc" },
    { Syntax::Language::Werk, "Werkfile" },
    { Syntax::Language::YAML, ".clang-format .clang-tidy .clangd .prettierrc .puppeteerrc .stylelintrc yarn.lock" },
};

bool contains_name(std::string_view names, std::string_view name) noexcept
{
    for (auto candidate: std::views::split(names, ' ')) {
        if (std::ranges::equal(candidate, name)) return true;
    }
    return false;
}

bool has_suffix(std::string_view filename, std::string_view suffix) noexcept
{
    return filename.size() > suffix.size()
        && filename.ends_with(suffix)
        && filename[filename.size() - suffix.size() - 1] == '.';
}

std::size_t matching_suffix_length(
    std::string_view suffixes,
    std::string_view filename
) noexcept
{
    std::size_t result = 0;
    for (auto suffix: std::views::split(suffixes, ' ')) {
        const std::string_view candidate{suffix};
        if (has_suffix(filename, candidate)) {
            result = std::max(result, candidate.size());
        }
    }
    return result;
}

constexpr const LanguageInfo* language_info(std::string_view name) noexcept
{
    auto lang = std::ranges::find(languages, name, &LanguageInfo::name);
    if (lang != std::end(languages)) return &*lang;
    auto alias = std::ranges::find(aliases, name, &LanguageAlias::name);
    if (alias != std::end(aliases)) return &language_info(alias->language);
    return nullptr;
}

std::size_t language_cache_index(Syntax::Language language)
{
    auto it = std::ranges::find(languages, language, &LanguageInfo::language);

    assert(it != std::end(languages));

    return it - std::begin(languages);
}

} // namespace

namespace {

constexpr std::optional<std::string_view> shebang_command(std::string_view text)
{
    auto is_blank = [](char c) { return c == ' ' || c == '\t'; };

    auto basename = [](std::string_view path) {
        while (path.ends_with('/')) path.remove_suffix(1);

        if (auto slash = path.find_last_of('/'); slash != std::string_view::npos)
            path.remove_prefix(slash + 1);

        return path;
    };

    struct Word {
        std::string_view text;
        std::size_t start; // offset in the pre-consumed input view
        bool quoted;
    };

    auto next_word = [&](std::string_view& s) -> std::optional<Word> {
        std::size_t i = 0;

        while (i < s.size() && is_blank(s[i])) ++i;

        if (i == s.size()) {
            s.remove_prefix(i);
            return std::nullopt;
        }

        if (s[i] == '\'' || s[i] == '"') {
            char quote = s[i++];
            std::size_t start = i;

            while (i < s.size() && s[i] != quote)
                ++i;

            auto word = s.substr(start, i - start);

            if (i < s.size()) ++i; // consume closing quote
            s.remove_prefix(i);
            return Word{word, start, true};
        }

        std::size_t start = i;

        while (i < s.size() && !is_blank(s[i]))
            ++i;

        auto word = s.substr(start, i - start);
        s.remove_prefix(i);

        return Word{word, start, false};
    };

    if (!text.starts_with("#!")) return std::nullopt;
    text.remove_prefix(2);
    auto end = text.find('\n');
    text = text.substr(0, end == std::string_view::npos ? text.size() : end);
    if (text.ends_with('\r')) text.remove_suffix(1);

    auto interpreter = next_word(text);
    if (!interpreter) return std::nullopt;

    auto name = basename(interpreter->text);

    if (name != "env") return name;

    // Parse arguments intended for /usr/bin/env.
    //
    // Handles common shebangs:
    //   #!/usr/bin/env python3
    //   #!/usr/bin/env -i python3
    //   #!/usr/bin/env FOO=bar python3
    //   #!/usr/bin/env -S python3 -O
    //   #!/usr/bin/env --split-string=python3 -O
    bool parsing_options = true;

    for (;;) {
        auto before = text;
        auto word = next_word(text);

        if (!word) return std::nullopt;

        auto w = word->text;

        auto restart_on_split_string = [&](std::size_t offset_in_word)
        {
            if (word->quoted)
                text = w.substr(offset_in_word);
            else
                text = before.substr(word->start + offset_in_word);

            parsing_options = true;
        };

        if (parsing_options) {
            if (w == "--") {
                parsing_options = false;
                continue;
            }

            // Historical env shorthand for -i.
            if (w == "-") continue;

            // GNU env -S / --split-string.
            if (w == "-S" || w == "--split-string") {
                // `text` already points at the remainder after this option.
                parsing_options = true;
                continue;
            }

            if (w.starts_with("--split-string=")) {
                restart_on_split_string(std::string_view{"--split-string="}.size());
                continue;
            }

            // Long env options without operands.
            if (w == "--ignore-environment" || w == "--null" || w == "--debug")
                continue;

            // Long env options with attached operands.
            if (w.starts_with("--unset=") ||
                w.starts_with("--chdir=") ||
                w.starts_with("--argv0=") ||
                w.starts_with("--path="))
                continue;

            // Long env options whose operand is the next word.
            if (w == "--unset" ||
                w == "--chdir" ||
                w == "--argv0" ||
                w == "--path") {
                if (!next_word(text)) return std::nullopt;

                continue;
            }

            // Short env options. Commonly:
            //   -i, -0, -v        no operand
            //   -u NAME, -C DIR   operand
            //   -S STRING         split string
            if (w.size() >= 2 && w[0] == '-' && w[1] != '-') {
                bool accepted = true;

                for (std::size_t j = 1; j < w.size(); ++j) {
                    char option = w[j];

                    if (option == 'i' || option == '0' || option == 'v')
                        continue;

                    if (option == 'S') {
                        if (j + 1 < w.size())
                            restart_on_split_string(j + 1);
                        else
                            parsing_options = true; // `text` is already the rest

                        accepted = true;
                        break;
                    }

                    if (option == 'u' || option == 'C' || option == 'a' || option == 'P') {
                        if (j + 1 == w.size() && !next_word(text))
                            return std::nullopt;

                        accepted = true;
                        break;
                    }

                    accepted = false;
                    break;
                }

                if (!accepted) return std::nullopt;

                continue;
            }
        }

        // env NAME=VALUE command
        if (auto equals = w.find('='); equals != std::string_view::npos && equals != 0) {
            parsing_options = false;
            continue;
        }

        return basename(w);
    }
}

// Not shebangs / empty cases
static_assert(shebang_command("") == std::nullopt, "empty input");
static_assert(shebang_command("echo hello\n") == std::nullopt, "not a shebang");
static_assert(shebang_command("# /bin/sh\n") == std::nullopt, "hash but not shebang");
static_assert(shebang_command("#!") == std::nullopt, "shebang only");
static_assert(shebang_command("#!   \t") == std::nullopt, "blank shebang");
static_assert(shebang_command("#!   \t\r\n") == std::nullopt, "blank shebang CRLF");

// Direct interpreters
static_assert(shebang_command("#!/bin/sh\n") == "sh", "basic /bin/sh");
static_assert(shebang_command("#!/bin/bash\n") == "bash", "basic /bin/bash");
static_assert(shebang_command("#!/usr/bin/python3") == "python3", "no trailing newline");
static_assert(shebang_command("#!/usr/bin/perl\r\n") == "perl", "CRLF line ending");
static_assert(shebang_command("#!/bin/bash -eu\n") == "bash", "arguments ignored for direct interpreter");
static_assert(shebang_command("#!/bin/bash\t-e\n") == "bash", "tab before argument");
static_assert(shebang_command("#!/usr/bin/ruby/\n") == "ruby", "trailing slash in interpreter path");
static_assert(shebang_command("#!'/usr/bin/node' --version\n") == "node", "single quoted interpreter");
static_assert(shebang_command("#!\"/opt/bin/my interp\" arg\n") == "my interp", "double quoted interpreter with space");

// env basics
static_assert(shebang_command("#!/usr/bin/env python3\n") == "python3", "env simple");
static_assert(shebang_command("#!/usr/bin/env /usr/local/bin/python3 -O\n") == "python3", "env with interpreter path");
static_assert(shebang_command("#!/usr/bin/env /opt/tool/\n") == "tool", "env path with trailing slash");
static_assert(shebang_command("#!/usr/bin/env/ python3\n") == "python3", "env path itself has trailing slash");
static_assert(shebang_command("#!/usr/bin/env\tpython3\n") == "python3", "env with tabs");

// env no-operand options
static_assert(shebang_command("#!/usr/bin/env -i python3\n") == "python3", "env -i");
static_assert(shebang_command("#!/usr/bin/env - python3\n") == "python3", "env historical dash");
static_assert(shebang_command("#!/usr/bin/env -iv0 python3\n") == "python3", "env combined short no-operand options");
static_assert(shebang_command("#!/usr/bin/env --ignore-environment python3\n") == "python3", "env --ignore-environment");
static_assert(shebang_command("#!/usr/bin/env --null python3\n") == "python3", "env --null");
static_assert(shebang_command("#!/usr/bin/env --debug python3\n") == "python3", "env --debug");
static_assert(shebang_command("#!/usr/bin/env --ignore-environment --debug --null python3\n") == "python3", "env several long no-operand options");

// env options with operands
static_assert(shebang_command("#!/usr/bin/env -u FOO python3\n") == "python3", "env -u separate");
static_assert(shebang_command("#!/usr/bin/env -uFOO python3\n") == "python3", "env -u attached");
static_assert(shebang_command("#!/usr/bin/env -C /tmp python3\n") == "python3", "env -C separate");
static_assert(shebang_command("#!/usr/bin/env -C/tmp python3\n") == "python3", "env -C attached");
static_assert(shebang_command("#!/usr/bin/env -a argv0 python3\n") == "python3", "env -a separate");
static_assert(shebang_command("#!/usr/bin/env -aargv0 python3\n") == "python3", "env -a attached");
static_assert(shebang_command("#!/usr/bin/env -P /bin:/usr/bin python3\n") == "python3", "env -P separate");
static_assert(shebang_command("#!/usr/bin/env -P/bin:/usr/bin python3\n") == "python3", "env -P attached");

static_assert(shebang_command("#!/usr/bin/env --unset=FOO python3\n") == "python3", "env --unset attached");
static_assert(shebang_command("#!/usr/bin/env --unset FOO python3\n") == "python3", "env --unset separate");
static_assert(shebang_command("#!/usr/bin/env --chdir=/tmp python3\n") == "python3", "env --chdir attached");
static_assert(shebang_command("#!/usr/bin/env --chdir /tmp python3\n") == "python3", "env --chdir separate");
static_assert(shebang_command("#!/usr/bin/env --argv0=py python3\n") == "python3", "env --argv0 attached");
static_assert(shebang_command("#!/usr/bin/env --argv0 py python3\n") == "python3", "env --argv0 separate");
static_assert(shebang_command("#!/usr/bin/env --path=/bin:/usr/bin python3\n") == "python3", "env --path attached");
static_assert(shebang_command("#!/usr/bin/env --path /bin:/usr/bin python3\n") == "python3", "env --path separate");

// env -- stops option parsing
static_assert(shebang_command("#!/usr/bin/env -- -weird\n") == "-weird", "env -- command beginning with dash");
static_assert(shebang_command("#!/usr/bin/env -- /usr/bin/python3 -O\n") == "python3", "env -- then path");

// env assignments
static_assert(shebang_command("#!/usr/bin/env FOO=bar python3\n") == "python3", "env assignment");
static_assert(shebang_command("#!/usr/bin/env FOO=bar A=B=C python3\n") == "python3", "env multiple assignments");
static_assert(shebang_command("#!/usr/bin/env FOO=bar -x\n") == "-x", "env assignment then dash command");
static_assert(shebang_command("#!/usr/bin/env =bad python3\n") == "=bad", "env equals at start is command");

// env -S / --split-string
static_assert(shebang_command("#!/usr/bin/env -S python3 -O\n") == "python3", "env -S separate");
static_assert(shebang_command("#!/usr/bin/env -Spython3 -O\n") == "python3", "env -S attached");
static_assert(shebang_command("#!/usr/bin/env --split-string python3 -O\n") == "python3", "env --split-string separate");
static_assert(shebang_command("#!/usr/bin/env --split-string=python3 -O\n") == "python3", "env --split-string attached");
static_assert(shebang_command("#!/usr/bin/env --split-string=/usr/local/bin/python3 -O\n") == "python3", "env --split-string attached path");
static_assert(shebang_command("#!/usr/bin/env '--split-string=python3 -O'\n") == "python3", "env quoted attached split-string");

// env missing command / invalid options
static_assert(shebang_command("#!/usr/bin/env\n") == std::nullopt, "env alone");
static_assert(shebang_command("#!/usr/bin/env -i\n") == std::nullopt, "env option but no command");
static_assert(shebang_command("#!/usr/bin/env -u\n") == std::nullopt, "env -u missing operand");
static_assert(shebang_command("#!/usr/bin/env --unset\n") == std::nullopt, "env --unset missing operand");
static_assert(shebang_command("#!/usr/bin/env -z python3\n") == std::nullopt, "env unknown short option");

struct ShebangLanguages
{
    Syntax::Language language;
    std::string_view commands;
    bool allow_version_suffix = false;
};

constexpr ShebangLanguages shebang_languages[] = {
    { Syntax::Language::Awk, "awk gawk mawk nawk" },
    { Syntax::Language::Bash, "sh bash dash zsh ksh mksh ash" },
    { Syntax::Language::Elixir, "elixir" },
    { Syntax::Language::Fish, "fish" },
    { Syntax::Language::JavaScript, "node nodejs", true },
    { Syntax::Language::Julia, "julia", true },
    { Syntax::Language::Lua, "lua luajit", true },
    { Syntax::Language::Nu, "nu" },
    { Syntax::Language::PHP, "php", true },
    { Syntax::Language::Perl, "perl", true },
    { Syntax::Language::PowerShell, "pwsh powershell" },
    { Syntax::Language::Python, "python", true },
    { Syntax::Language::R, "R Rscript" },
    { Syntax::Language::Ruby, "ruby", true },
    { Syntax::Language::TCL, "tclsh tclish jimsh wish" },
};

constexpr bool is_version_suffix(std::string_view suffix) noexcept
{
    if (suffix.empty() || suffix.front() < '0' || suffix.front() > '9')
        return false;
    if (suffix.back() < '0' || suffix.back() > '9') return false;

    return std::ranges::all_of(suffix, [](char c) {
        return (c >= '0' && c <= '9') || c == '.';
    });
}

constexpr bool command_matches(
    std::string_view commands, std::string_view command,
    bool allow_version_suffix
) noexcept
{
    for (auto part: std::views::split(commands, ' ')) {
        const std::string_view candidate{part};
        if (command == candidate) return true;
        if (allow_version_suffix && command.starts_with(candidate) &&
            is_version_suffix(command.substr(candidate.size())))
            return true;
    }
    return false;
}

constexpr std::optional<Syntax::Language>
language_from_shebang_command(std::string_view command) noexcept
{
    for (auto [language, commands, allow_version_suffix]: shebang_languages) {
        if (command_matches(commands, command, allow_version_suffix))
            return language;
    }
    return std::nullopt;
}

static_assert(language_from_shebang_command("sh") == Syntax::Language::Bash);
static_assert(language_from_shebang_command("python3.12") == Syntax::Language::Python);
static_assert(language_from_shebang_command("node20") == Syntax::Language::JavaScript);
static_assert(language_from_shebang_command("ruby3.3") == Syntax::Language::Ruby);
static_assert(language_from_shebang_command("perl5.40") == Syntax::Language::Perl);
static_assert(language_from_shebang_command("php8.3") == Syntax::Language::PHP);
static_assert(language_from_shebang_command("lua5.4") == Syntax::Language::Lua);
static_assert(language_from_shebang_command("Rscript") == Syntax::Language::R);
static_assert(language_from_shebang_command("gawk") == Syntax::Language::Awk);
static_assert(language_from_shebang_command("fish") == Syntax::Language::Fish);
static_assert(language_from_shebang_command("elixir") == Syntax::Language::Elixir);
static_assert(language_from_shebang_command("julia1.12") == Syntax::Language::Julia);
static_assert(language_from_shebang_command("nu") == Syntax::Language::Nu);
static_assert(language_from_shebang_command("pwsh") == Syntax::Language::PowerShell);
static_assert(language_from_shebang_command("tclsh") == Syntax::Language::TCL);
static_assert(language_from_shebang_command("python-next") == std::nullopt);

} // namespace

std::string_view canonical_name(Syntax::Language language) noexcept
{
    return language_info(language).name;
}

std::span<const Syntax::Language> Syntax::available_languages() noexcept
{
    static constexpr auto values = [] {
        std::array<Syntax::Language, std::size(languages)> result{};

        for (auto [info, lang]: std::views::zip(languages, result)) {
            lang = info.language;
        };

        return result;
    }();

    return values;
}

std::optional<Syntax::Language> Syntax::language_from(std::string_view name) noexcept
{
    auto info = language_info(name);
    if (!info) return std::nullopt;
    return info->language;
}

std::optional<Syntax::Language> Syntax::language_from(std::filesystem::path path)
{
    const auto filename = path.filename().string();

    for (auto [language, names]: basename_languages) {
        if (contains_name(names, filename)) return language;
    }

    std::optional<Language> result;
    std::size_t longest_suffix = 0;
    for (auto [language, suffixes]: suffix_languages) {
        const auto length = matching_suffix_length(suffixes, filename);
        if (length > longest_suffix) {
            result = language;
            longest_suffix = length;
        }
    }
    return result;
}

std::optional<Syntax::Language>
Syntax::language_from_shebang(std::u8string_view text) noexcept
{
    const std::string_view bytes{
        reinterpret_cast<const char*>(text.data()), text.size()
    };
    return shebang_command(bytes).and_then(language_from_shebang_command);
}

namespace {

struct TSQueryDeleter
{
    void operator()(TSQuery* query) const noexcept { ts_query_delete(query); }
};

using TSQueryPtr = std::unique_ptr<TSQuery, TSQueryDeleter>;

} // namespace

} // namespace mlang

auto operator<=>(TSPoint a, TSPoint b)
{
    return a.row != b.row ? a.row <=> b.row : a.column <=> b.column;
}

bool operator==(TSPoint a, TSPoint b)
{
    return (a <=> b) == 0;
}

bool operator==(TSRange a, TSRange b)
{
    return a.start_byte == b.start_byte &&
           a.end_byte == b.end_byte &&
           a.start_point == b.start_point &&
           a.end_point == b.end_point;
}

namespace mlang {

namespace {

bool edit_touches_ranges(TSInputEdit edit, std::span<const TSRange> ranges)
{
    return std::ranges::any_of(ranges, [=](TSRange range) {
        // Replacement/deletion.
        if (edit.start_byte != edit.old_end_byte) {
            return edit.start_byte < range.end_byte &&
                   edit.old_end_byte > range.start_byte;
        }

        // Pure insertion. Treat insertion at either boundary as modifying the layer.
        return range.start_byte <= edit.start_byte &&
               edit.start_byte <= range.end_byte;
    });
}

constexpr const TSRange ts_max_range = {
    .start_point = TSPoint { .row = 0, .column = 0 },
    .end_point = TSPoint {
        .row = std::numeric_limits<decltype(TSPoint{}.row)>::max(),
        .column = std::numeric_limits<decltype(TSPoint{}.column)>::max(),
    },
    .start_byte = 0,
    .end_byte = std::numeric_limits<decltype(TSRange{}.end_byte)>::max()
};

} // namespace

struct Layer
{
    Syntax::Language language;
    const TSLanguage* ts_language;
    std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree {
        nullptr, ts_tree_delete
    };

    // Always in root-document coordinates.
    std::vector<TSRange> included_ranges;

    // True if the bytes visible to this layer changed.
    bool modified = true;

    Layer* parent = nullptr;
    std::vector<std::unique_ptr<Layer>> children;

    Layer(Syntax::Language language, std::vector<TSRange> ranges, Layer* parent)
    : language{language}
    , ts_language{language_info(language).make()}
    , included_ranges{std::move(ranges)}
    , parent{parent}
    {}

    void apply_edit(TSInputEdit edit)
    {
        if (tree) ts_tree_edit(tree.get(), &edit);

        if (edit_touches_ranges(edit, included_ranges)) {
            modified = true;
        }

        for (TSRange& range: included_ranges) ts_range_edit(&range, &edit);
        for (auto& child: children) child->apply_edit(edit);
    }
};

namespace {

struct Injection
{
    Syntax::Language language;
    std::vector<TSRange> included_ranges;
};

TSRange range_of_node(TSNode node)
{
    return {
        .start_point = ts_node_start_point(node),
        .end_point = ts_node_end_point(node),
        .start_byte = ts_node_start_byte(node),
        .end_byte = ts_node_end_byte(node),
    };
}

constexpr bool is_empty_range(TSRange range) noexcept
{
    return range.start_byte >= range.end_byte;
}

bool normalize_ranges(std::vector<TSRange>& ranges)
{
    std::erase_if(ranges, is_empty_range);

    std::ranges::sort(ranges, {}, &TSRange::start_byte);

    std::vector<TSRange> normalized;
    normalized.reserve(ranges.size());

    for (TSRange range : ranges) {
        if (normalized.empty()) {
            normalized.push_back(range);
            continue;
        }

        TSRange& previous = normalized.back();

        if (range == previous) {
            continue;
        }

        if (range.start_byte < previous.end_byte) {
            return false;
        }

        if (range.start_byte == previous.end_byte) {
            if (range.start_point != previous.end_point) {
                return false;
            }

            previous.end_byte = range.end_byte;
            previous.end_point = range.end_point;
            continue;
        }

        normalized.push_back(range);
    }

    ranges = std::move(normalized);
    return true;
}

std::vector<TSRange> ranges_for_content_node(
    TSNode node,
    bool include_children,
    bool include_unnamed_children
)
{
    if (include_children) {
        TSRange range = range_of_node(node);
        return is_empty_range(range) ? std::vector<TSRange>{}
                                     : std::vector<TSRange>{range};
    }

    std::vector<TSRange> ranges;

    uint32_t cursor_byte = ts_node_start_byte(node);
    TSPoint cursor_point = ts_node_start_point(node);

    auto append_gap = [&](TSPoint end_point, uint32_t end_byte) {
        if (cursor_byte < end_byte) {
            ranges.push_back({
                .start_point = cursor_point,
                .end_point = end_point,
                .start_byte = cursor_byte,
                .end_byte = end_byte,
            });
        }

        cursor_point = end_point;
        cursor_byte = end_byte;
    };

    const uint32_t child_count = ts_node_child_count(node);

    for (uint32_t i = 0; i < child_count; ++i) {
        TSNode child = ts_node_child(node, i);

        bool exclude_child =
            include_unnamed_children
                ? ts_node_is_named(child)
                : true;

        if (!exclude_child) {
            continue;
        }

        uint32_t child_start_byte = ts_node_start_byte(child);
        uint32_t child_end_byte = ts_node_end_byte(child);

        TSPoint child_start_point = ts_node_start_point(child);
        TSPoint child_end_point = ts_node_end_point(child);

        if (child_end_byte <= cursor_byte) {
            continue;
        }

        if (cursor_byte < child_start_byte) {
            append_gap(child_start_point, child_start_byte);
        }

        cursor_point = child_end_point;
        cursor_byte = child_end_byte;
    }

    append_gap(ts_node_end_point(node), ts_node_end_byte(node));

    return ranges;
}

std::u8string_view node_text(std::u8string_view source, TSNode node)
{
    const auto start = ts_node_start_byte(node), end = ts_node_end_byte(node);

    return source.substr(start, end - start);
}

std::string_view capture_name(
    const TSQuery* query,
    uint32_t capture_id
)
{
    uint32_t length = 0;
    const char* name = ts_query_capture_name_for_id(query, capture_id, &length);
    return {name, length};
}

std::string_view string_value(const TSQuery* query, uint32_t string_id)
{
    uint32_t length = 0;
    const char* value = ts_query_string_value_for_id(query, string_id, &length);
    return {value, length};
}

struct PredicateExpression
{
    std::string_view name;
    std::span<const TSQueryPredicateStep> steps;
};

std::vector<PredicateExpression> predicates(const TSQuery* query, TSQueryMatch match)
{
    std::vector<PredicateExpression> result;

    uint32_t step_count = 0;
    auto steps = ts_query_predicates_for_pattern(query, match.pattern_index, &step_count);
    size_t start = 0, count = 0;
    for (uint32_t i = 0; i < step_count; i++) {
        auto& step = steps[i];
        if (step.type == TSQueryPredicateStepTypeDone) {
            std::span<const TSQueryPredicateStep> predicate(steps + start, count);

            assert(!predicate.empty());
            assert(predicate.front().type == TSQueryPredicateStepTypeString);

            result.push_back({
                .name = string_value(query, predicate.front().value_id),
                .steps = predicate.subspan(1)
            });
            start = i + 1; count = 0;
        } else count += 1;
    }

    return result;
}

std::string_view as_bytes(std::u8string_view text) noexcept
{
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::optional<std::string_view> predicate_step_text(
    const TSQuery* query,
    TSQueryMatch match,
    const TSQueryPredicateStep& step,
    std::u8string_view source
)
{
    switch (step.type) {
    case TSQueryPredicateStepTypeString:
        return string_value(query, step.value_id);

    case TSQueryPredicateStepTypeCapture:
        for (uint32_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];

            if (capture.index == step.value_id) {
                return as_bytes(node_text(source, capture.node));
            }
        }

        return std::nullopt;

    case TSQueryPredicateStepTypeDone:
        return std::nullopt;
    }

    return std::nullopt;
}

bool eval_predicate(
    const TSQuery* query, TSQueryMatch match,
    const PredicateExpression& expr, std::u8string_view source
)
{
    if (expr.name == "any-of?" || expr.name == "not-any-of?") {
        const auto negated = expr.name == "not-any-of?";
        // Expected form:
        //
        //   (#any-of? @capture "a" "b" "c")
        //   (#not-any-of? @capture "a" "b" "c")
        //
        // So we need at least one value to test and one candidate.
        if (expr.steps.size() < 2) return false;

        auto needle = predicate_step_text(query, match, expr.steps[0], source);
        if (!needle) return false;

        auto haystack = expr.steps.subspan(1);

        bool found =
            std::ranges::find(haystack, needle,
                [&](const TSQueryPredicateStep& step) {
                    return predicate_step_text(query, match, step, source);
                }
            ) != haystack.end();

        return negated ? !found : found;
    }
    if (expr.name == "eq?" || expr.name == "not-eq?") {
        const auto negated = expr.name == "not-eq?";
        if (expr.steps.size() != 2) return false;

        auto lhs = predicate_step_text(query, match, expr.steps[0], source);
        auto rhs = predicate_step_text(query, match, expr.steps[1], source);

        if (!lhs || !rhs) return false;

        bool equal = *lhs == *rhs;
        return negated ? !equal : equal;
    }
    if (expr.name == "match?" || expr.name == "not-match?") {
        const auto negated = expr.name == "not-match?";

        // Expected form:
        //
        //   (#match? @capture "regex")
        //   (#not-match? @capture "regex")
        //
        // Tree-sitter-style match predicates test whether the regex matches
        // anywhere in the captured text. Anchors like ^ and $ can be used by
        // the query when full-string matching is desired.
        if (expr.steps.size() != 2) return false;

        auto lhs = predicate_step_text(query, match, expr.steps[0], source);
        auto pattern = predicate_step_text(query, match, expr.steps[1], source);

        if (!lhs || !pattern) return false;

        try {
            std::regex regex(pattern->begin(), pattern->end(), std::regex::ECMAScript);

            bool matched = std::regex_search(lhs->begin(), lhs->end(), regex);

            return negated ? !matched : matched;
        } catch (const std::regex_error&) {
            // Treat malformed regex predicates as failed.
            return false;
        }
    }

    return true;
}

std::vector<Injection> discover_injections(
    const Layer& layer, std::u8string_view source, const TSQuery* query
)
{
    std::vector<Injection> result;

    if (!layer.tree) return result;
    if (!query) return result;

    std::unique_ptr<TSQueryCursor, decltype(&ts_query_cursor_delete)> cursor {
        ts_query_cursor_new(), ts_query_cursor_delete
    };

    ts_query_cursor_exec(cursor.get(), query,
        ts_tree_root_node(layer.tree.get())
    );

    struct CombinedGroup
    {
        Syntax::Language language;
        std::vector<TSRange> ranges;
    };

    std::vector<CombinedGroup> combined_groups;

    TSQueryMatch match;

    while (ts_query_cursor_next_match(cursor.get(), &match)) {
        bool skip = false;
        std::optional<Syntax::Language> injection_language;
        std::optional<Syntax::Language> shebang_language;

        std::vector<TSNode> content_nodes;

        bool combined = false;
        bool include_children = false;
        bool include_unnamed_children = false;

        // Read #set! directives for this pattern.
        for (auto predicate: predicates(query, match)) {
            if (!eval_predicate(query, match, predicate, source)) {
                skip = true;
                break;
            }
            if (predicate.name == "set!") {
                assert(!predicate.steps.empty());
                const TSQueryPredicateStep& property = predicate.steps.front();
                if (property.type == TSQueryPredicateStepTypeString) {
                    auto key = string_value(query, property.value_id);
                    if (key == "injection.language" && predicate.steps.size() == 2) {
                        if (predicate.steps[1].type == TSQueryPredicateStepTypeString) {
                            auto name = string_value(query, predicate.steps[1].value_id);
                            if (const LanguageInfo* info = language_info(name)) {
                                injection_language = info->language;
                            } else {
                                injection_language.reset();
                            }
                        }
                    } else if (key == "injection.combined") {
                        combined = true;
                    } else if (key == "injection.include-children") {
                        include_children = true;
                    } else if (key == "injection.include-unnamed-children") {
                        include_unnamed_children = true;
                    }
                }
            }
        }

        // Read captures.
        for (uint32_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];
            std::string_view name = capture_name(query, capture.index);

            if (name == "injection.content") {
                content_nodes.push_back(capture.node);
            } else if (name == "injection.language") {
                auto text = node_text(source, capture.node);
                if (const LanguageInfo* info = language_info(as_bytes(text))) {
                    injection_language = info->language;
                } else {
                    injection_language.reset();
                }
            } else if (name == "injection.shebang") {
                if (!shebang_language) {
                    shebang_language = Syntax::language_from_shebang(
                        node_text(source, capture.node)
                    );
                }
            }
        }

        if (!injection_language) injection_language = shebang_language;

        if (content_nodes.empty()) continue;

        if (!injection_language) continue;

        std::vector<TSRange> ranges;

        for (TSNode content_node: content_nodes) {
            auto node_ranges = ranges_for_content_node(content_node,
                include_children,
                include_unnamed_children
            );

            ranges.insert(ranges.end(), node_ranges.begin(), node_ranges.end());
        }

        if (!normalize_ranges(ranges)) continue;
        if (ranges.empty()) continue;

        if (combined) {
            auto it = std::ranges::find(
                combined_groups, *injection_language,
                &CombinedGroup::language
            );

            if (it == combined_groups.end()) {
                combined_groups.push_back({
                    .language = *injection_language,
                    .ranges = std::move(ranges)
                });
            } else {
                it->ranges.insert(
                    it->ranges.end(),
                    ranges.begin(),
                    ranges.end()
                );
            }
        } else {
            result.push_back({
                .language = *injection_language,
                .included_ranges = std::move(ranges),
            });
        }
    }

    for (CombinedGroup& group: combined_groups) {
        if (!normalize_ranges(group.ranges)) continue;
        if (group.ranges.empty()) continue;

        result.push_back({
            .language = group.language,
            .included_ranges = std::move(group.ranges)
        });
    }

    return result;
}

Layer& append_reused_or_new_child(
    std::vector<std::unique_ptr<Layer>>& next_children,
    std::vector<std::unique_ptr<Layer>>& old_children,
    Layer& parent,
    Syntax::Language language,
    std::span<const TSRange> ranges
) {
    auto it = std::ranges::find_if(old_children, [&](const auto& child) {
        assert(child);
        assert(child->parent == &parent);
        return child->language == language &&
               std::ranges::equal(child->included_ranges, ranges);
    });

    std::unique_ptr<Layer> child;

    if (it != old_children.end()) {
        child = std::move(*it);
        old_children.erase(it);
    } else {
        child = std::make_unique<Layer>(
            language,
            std::vector(ranges.begin(), ranges.end()),
            &parent
        );

        child->modified = true;
    }

    auto& ref = *child;
    next_children.push_back(std::move(child));
    return ref;
}

} // namespace

namespace {

constexpr TSPoint to_ts(Syntax::Point point) noexcept
{
    return {
        .row = point.row,
        .column = point.byte_column,
    };
}

constexpr Syntax::Point from_ts(TSPoint point) noexcept
{
    return {
        .row = point.row,
        .byte_column = point.column,
    };
}

constexpr Syntax::Region from_ts(TSRange range) noexcept
{
    return {
        .start = {
            .byte = {.index = range.start_byte},
            .point = from_ts(range.start_point),
        },
        .end = {
            .byte = {.index = range.end_byte},
            .point = from_ts(range.end_point),
        },
    };
}

constexpr TSRange to_ts(Syntax::Region region) noexcept
{
    return {
        .start_point = to_ts(region.start.point),
        .end_point = to_ts(region.end.point),
        .start_byte = region.start.byte.index,
        .end_byte = region.end.byte.index,
    };
}

constexpr TSInputEdit to_ts(Syntax::Edit edit) noexcept
{
    return {
        .start_byte = edit.old.start.byte.index,
        .old_end_byte = edit.old.end.byte.index,
        .new_end_byte = edit.new_end.byte.index,

        .start_point = to_ts(edit.old.start.point),
        .old_end_point = to_ts(edit.old.end.point),
        .new_end_point = to_ts(edit.new_end.point),
    };
}

Syntax::Region region_of(TSNode node) noexcept
{
    return {
        .start = {
            .byte = {.index = ts_node_start_byte(node)},
            .point = from_ts(ts_node_start_point(node)),
        },
        .end = {
            .byte = {.index = ts_node_end_byte(node)},
            .point = from_ts(ts_node_end_point(node)),
        }
    };
}

} // namespace

namespace {

const char *read_input(
    void *payload,
    uint32_t byte_index,
    TSPoint,
    uint32_t *bytes_read
) {
    auto &input = *static_cast<const std::u8string_view *>(payload);

    if (byte_index >= input.size()) {
        *bytes_read = 0;
        return "";
    }

    *bytes_read = input.size() - byte_index;
    return reinterpret_cast<const char *>(input.data() + byte_index);
}

bool timeout_callback(TSParseState *state)
{
    auto &deadline = *static_cast<const std::chrono::steady_clock::time_point *>(state->payload);
    return std::chrono::steady_clock::now() >= deadline;
}

} // namespace

namespace {

template<class Contains>
const Layer& innermost_layer_for(const Layer& layer, Contains contains)
{
    const Layer* best = nullptr;
    uint32_t best_width = 0;

    for (const auto& child : layer.children) {
        auto it = std::ranges::find_if(child->included_ranges, contains);
        if (it == child->included_ranges.end()) continue;

        uint32_t width = it->end_byte - it->start_byte;

        // Smaller range wins; for equal ranges, later sibling wins.
        if (!best || width <= best_width) {
            best = child.get();
            best_width = width;
        }
    }

    return best ? innermost_layer_for(*best, contains) : layer;
}

} // namespace

struct Syntax::Impl
{
    Impl(Language language)
    : root{language, std::vector{ts_max_range}, nullptr}
    {}

    Layer root;
    std::array<TSQueryPtr, std::size(languages)> injection_query_cache{};
    const TSQuery* injection_query_for(Language language)
    {
        const auto& source = injections_query_source[static_cast<std::size_t>(language)];

        if (source.empty()) return nullptr;

        auto& cached = injection_query_cache[language_cache_index(language)];

        if (!cached) {
            const auto& info = language_info(language);
            uint32_t error_offset = 0;
            TSQueryError error_type{};

            TSQuery* query = ts_query_new(
                info.make(), source.data(), source.size(),
                &error_offset, &error_type
            );

            if (!query) {
                throw std::runtime_error(
                    "Query compilation failed for language `" +
                    std::string(info.name) +
                    "` at offset " +
                    std::to_string(error_offset)
                );
            }

            cached.reset(query);
        }

        return cached.get();
    }

    const Layer& layer_for(ByteRange range) const
    {
        auto contains = [range](TSRange ts) {
            ByteRange layer_range {
                .start = ByteIndex{ts.start_byte},
                .end = ByteIndex{ts.end_byte},
            };

            return range.empty()
                ? layer_range.contains(range.start)
                : layer_range.contains(range);
        };

        return innermost_layer_for(root, contains);
    }

    const Layer& layer_for(Syntax::PointRange range) const
    {
        auto contains = [range](TSRange ts) {
            PointRange layer_range {
                .start = from_ts(ts.start_point),
                .end = from_ts(ts.end_point),
            };

            return range.empty()
                ? layer_range.contains(range.start)
                : layer_range.contains(range);
        };

        return innermost_layer_for(root, contains);
    }

    std::unique_ptr<TSTree, decltype(&ts_tree_delete)> parse_layer(
        std::u8string_view source, const TSLanguage* language,
        std::span<const TSRange> included_ranges, TSTree* old_tree
    )
    {
        if (!ts_parser_set_language(parser.get(), language))
            throw std::runtime_error("Failed to set Tree-sitter language");
        ts_parser_set_included_ranges(parser.get(), included_ranges.data(), included_ranges.size());

        TSInput input {
            .payload = &source, .read = read_input, .encoding = TSInputEncodingUTF8
        };

        auto deadline = std::chrono::steady_clock::now() + parse_timeout;
        TSParseOptions options {
            .payload = &deadline, .progress_callback = timeout_callback
        };

        return {
            ts_parser_parse_with_options(parser.get(), old_tree, input, options),
            ts_tree_delete
        };
    }

    bool reparse_modified_layers(Layer& layer, std::u8string_view source)
    {
        if (layer.modified) {
            auto new_tree = parse_layer(
                source,
                layer.ts_language,
                layer.included_ranges,
                layer.tree.get()
            );

            // If parsing times out, Tree-sitter retains resumable parser state.
            // This is safe even though we reuse one TSParser for all layers:
	    // on timeout we return immediately, leave this layer modified,
	    // and do not parse any later sibling/child layer.
	    // On continue_parsing(), all ancestors are already
            // unmodified, so traversal reaches this same layer first,
	    // with the same language, included ranges, and old tree.
	    // Therefore the interrupted parse is resumed/restarted at the
	    // same layer by construction.
            if (!new_tree) return false;

            layer.tree = std::move(new_tree);
            layer.modified = false;
            auto old_children = std::move(layer.children);
            std::vector<std::unique_ptr<Layer>> next_children;

            auto injections = discover_injections(layer, source, injection_query_for(layer.language));
            next_children.reserve(injections.size());

            for (const Injection& injection: injections) {
                assert(!injection.included_ranges.empty());

                append_reused_or_new_child(
                    next_children,
                    old_children,
                    layer,
                    injection.language,
                    injection.included_ranges
                );
            }

            layer.children = std::move(next_children);
        }

        for (auto& child: layer.children)
            if (!reparse_modified_layers(*child, source)) return false;

        return true;
    }

    bool edit(std::u8string_view new_source, std::span<const Edit> edits)
    {
        if (edits.empty()) {
            return reparse_modified_layers(root, new_source);
        }

        // In case our first parse timed out, an edit invalidates the resume state.
        if (!root.tree) ts_parser_reset(parser.get());

        for (const Edit edit: edits) {
            root.apply_edit(to_ts(edit));
        }

        root.included_ranges = { ts_max_range };
        root.modified = true;

        return reparse_modified_layers(root, new_source);
    }

    std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser {
        ts_parser_new(), ts_parser_delete
    };
    std::chrono::steady_clock::duration parse_timeout = std::chrono::milliseconds{500};
};

Syntax::Syntax(std::u8string_view source, Language language)
: impl{std::make_unique<Impl>(language)}
{
    impl->reparse_modified_layers(impl->root, source);
}

Syntax::~Syntax() = default;
Syntax::Syntax(Syntax&&) noexcept = default;
Syntax& Syntax::operator=(Syntax&&) noexcept = default;

Syntax::operator bool() const noexcept
{
    auto completed = [](this auto&& self, const Layer& layer) -> bool
    {
        if (layer.modified) return false;

        for (const auto& child: layer.children)
            if (!self(*child)) return false;

        return true;
    };

    return completed(impl->root);
}

bool Syntax::continue_parsing(std::u8string_view source)
{
    return impl->reparse_modified_layers(impl->root, source);
}

void Syntax::dump_layers(std::ostream& out) const
{
    auto dump =
        [&](this auto&& self, const Layer& layer, int depth = 0) -> void
    {
        out << std::string(depth * 2, ' ')
            << language_info(layer.language).name;

        if (!layer.tree) {
            out << " <no tree>";
        }

        out << " ranges:";
        for (TSRange range : layer.included_ranges) {
            out << " [" << range.start_byte << ", "
                << range.end_byte << ")";
        }

        out << '\n';

        for (const auto& child : layer.children) {
            self(*child, depth + 1);
        }
    };

    dump(impl->root);
}

struct Syntax::Node::Factory
{
    static std::optional<Syntax::Node> make(const Layer& layer, TSNode node);
};

struct Syntax::Node::Impl
{
    const Layer* layer;
    TSNode node;

    Impl(const Layer& layer, TSNode node)
    : layer{&layer}, node{node}
    {}

    Impl(const Impl&) = default;
    Impl& operator=(const Impl&) = default;

    Impl(Impl&&) = default;
    Impl& operator=(Impl&&) = default;

    template <typename... Args>
    std::optional<Node> map_node(TSNode (*f)(TSNode, Args...), Args... args) const
    {
        return Node::Factory::make(*layer, f(node, args...));
    }
};

Syntax::Node::Node(Impl&& impl) noexcept
{
    static_assert(sizeof(Impl) <= impl_size);
    static_assert(alignof(Impl) <= impl_align);

    std::construct_at(
        reinterpret_cast<Impl*>(storage),
        std::move(impl)
    );
}

Syntax::Node::Impl* Syntax::Node::impl() noexcept
{
    return std::launder(reinterpret_cast<Impl*>(storage));
}

const Syntax::Node::Impl* Syntax::Node::impl() const noexcept
{
    return std::launder(reinterpret_cast<const Impl*>(storage));
}

Syntax::Node::~Node() noexcept
{
    std::destroy_at(impl());
}

Syntax::Node::Node(Node&& other) noexcept
{
    std::construct_at(
        reinterpret_cast<Impl*>(storage),
        std::move(*other.impl())
    );
}

Syntax::Node& Syntax::Node::operator=(Node&& other) noexcept
{
    if (this != &other) *impl() = std::move(*other.impl());

    return *this;
}

Syntax::Node::Node(const Node& other) noexcept
{
    std::construct_at(
        reinterpret_cast<Impl*>(storage),
        *other.impl()
    );
}

Syntax::Node& Syntax::Node::operator=(const Node& other) noexcept
{
    if (this != &other) *impl() = *other.impl();

    return *this;
}

Syntax::Language Syntax::Node::language() const noexcept
{ return impl()->layer->language; }

std::string_view Syntax::Node::type() const noexcept
{ return {ts_node_type(impl()->node)}; }

bool Syntax::Node::is_named() const noexcept
{ return ts_node_is_named(impl()->node); }

std::optional<Syntax::Node>
Syntax::Node::Factory::make(const Layer& layer, TSNode ts_node)
{
    if (ts_node_is_null(ts_node)) return std::nullopt;

    return Node{Impl{layer, ts_node}};
}

std::optional<Syntax::Node> Syntax::root() const noexcept
{
    if (!impl->root.tree) return std::nullopt;
    return Node::Factory::make(
        impl->root, ts_tree_root_node(impl->root.tree.get())
    );
}

std::optional<Syntax::Node> Syntax::descendant_for(ByteRange range) const
{
    const Layer& layer = impl->layer_for(range);

    if (!layer.tree) return std::nullopt;

    TSNode node = ts_node_descendant_for_byte_range(
        ts_tree_root_node(layer.tree.get()), range.start.index, range.end.index
    );

    return Node::Factory::make(layer, node);
}

std::optional<Syntax::Node> Syntax::descendant_for(PointRange range) const
{
    const Layer& layer = impl->layer_for(range);

    if (!layer.tree) return std::nullopt;

    TSNode node = ts_node_descendant_for_point_range(
        ts_tree_root_node(layer.tree.get()),
        to_ts(range.start), to_ts(range.end)
    );

    return Node::Factory::make(layer, node);
}

std::optional<Syntax::Node> Syntax::named_descendant_for(ByteRange range) const
{
    const Layer& layer = impl->layer_for(range);

    if (!layer.tree) return std::nullopt;

    TSNode node = ts_node_named_descendant_for_byte_range(
        ts_tree_root_node(layer.tree.get()), range.start.index, range.end.index
    );

    return Node::Factory::make(layer, node);
}

std::optional<Syntax::Node> Syntax::named_descendant_for(PointRange range) const
{
    const Layer& layer = impl->layer_for(range);

    if (!layer.tree) return std::nullopt;

    TSNode node = ts_node_named_descendant_for_point_range(
        ts_tree_root_node(layer.tree.get()),
        to_ts(range.start), to_ts(range.end)
    );

    return Node::Factory::make(layer, node);
}

std::u8string_view Syntax::Node::text(std::u8string_view source) const noexcept
{
    assert(ts_node_end_byte(impl()->node) <= source.size());
    return node_text(source, impl()->node);
}

Syntax::Region Syntax::Node::range() const noexcept
{
    return region_of(impl()->node);
}

std::optional<Syntax::Node> Syntax::Node::parent() const
{
    return impl()->map_node(ts_node_parent).or_else([this] -> std::optional<Node> {
        const Layer* parent_layer = impl()->layer->parent;

        if (!parent_layer || !parent_layer->tree) return std::nullopt;

        assert(!impl()->layer->included_ranges.empty());

        auto region = range();
        TSNode parent_node = ts_node_descendant_for_byte_range(
            ts_tree_root_node(parent_layer->tree.get()),
            region.start.byte.index, region.end.byte.index
        );

        if (region_of(parent_node) == region) {
            parent_node = ts_node_parent(parent_node);
        }

        return Factory::make(*parent_layer, parent_node);
    });
}

std::optional<Syntax::Node> Syntax::Node::next_sibling() const
{
    return impl()->map_node(ts_node_next_sibling);
}

std::optional<Syntax::Node> Syntax::Node::previous_sibling() const
{
    return impl()->map_node(ts_node_prev_sibling);
}

std::optional<Syntax::Node> Syntax::Node::next_named_sibling() const
{
    return impl()->map_node(ts_node_next_named_sibling);
}

std::optional<Syntax::Node> Syntax::Node::previous_named_sibling() const
{
    return impl()->map_node(ts_node_prev_named_sibling);
}

Syntax::Node::Children Syntax::Node::children() const noexcept
{
    return {*this, false};
}

Syntax::Node::Children Syntax::Node::named_children() const noexcept
{
    return {*this, true};
}

Syntax::Node::Children::Children(Node node, bool named) noexcept
: node{std::move(node)}, named{named}
{}

Syntax::Node::Children::iterator::iterator(
    const Children* owner, std::ptrdiff_t index
) noexcept
: owner{owner}, index{index}
{}

Syntax::Node Syntax::Node::Children::iterator::operator*() const noexcept
{
    assert(owner);

    const Node::Impl* impl = owner->node.impl();

    auto result = impl->map_node(
        owner->named ? ts_node_named_child : ts_node_child,
        static_cast<uint32_t>(index)
    );

    assert(result);
    return std::move(*result);
}

Syntax::Node Syntax::Node::Children::iterator::operator[](
    difference_type n
) const noexcept
{
    return *(*this + n);
}

Syntax::Node::Children::iterator&
Syntax::Node::Children::iterator::operator++() noexcept
{
    ++index;
    return *this;
}

Syntax::Node::Children::iterator
Syntax::Node::Children::iterator::operator++(int) noexcept
{
    iterator old = *this;
    ++*this;
    return old;
}

Syntax::Node::Children::iterator&
Syntax::Node::Children::iterator::operator--() noexcept
{
    --index;
    return *this;
}

Syntax::Node::Children::iterator
Syntax::Node::Children::iterator::operator--(int) noexcept
{
    iterator old = *this;
    --*this;
    return old;
}

Syntax::Node::Children::iterator&
Syntax::Node::Children::iterator::operator+=(difference_type n) noexcept
{
    index += n;
    return *this;
}

Syntax::Node::Children::iterator&
Syntax::Node::Children::iterator::operator-=(difference_type n) noexcept
{
    index -= n;
    return *this;
}

Syntax::Node::Children::iterator
Syntax::Node::Children::begin() const noexcept
{
    return iterator{this, 0};
}

Syntax::Node::Children::iterator
Syntax::Node::Children::end() const noexcept
{
    return iterator{this, static_cast<iterator::difference_type>(size())};
}

std::size_t Syntax::Node::Children::size() const noexcept
{
    const auto f = named ? ts_node_named_child_count : ts_node_child_count;
    return f(node.impl()->node);
}

static_assert(std::ranges::view<Syntax::Node::Children>);
static_assert(std::ranges::sized_range<Syntax::Node::Children>);
static_assert(std::ranges::random_access_range<Syntax::Node::Children>);

bool Syntax::edit(std::u8string_view new_source, Edit edit)
{
    return this->edit(new_source, std::span<const Edit>(&edit, 1));
}

bool Syntax::edit(std::u8string_view new_source, std::span<const Edit> edits)
{
    return impl->edit(new_source, edits);
}

} // namespace mlang
