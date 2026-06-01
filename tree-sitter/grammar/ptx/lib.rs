use tree_sitter::Language;

extern "C" {
    fn tree_sitter_ptx() -> Language;
}

pub fn language() -> Language {
    unsafe { tree_sitter_ptx() }
}

pub const HIGHLIGHTS_QUERY: &str = include_str!("../queries/highlights.scm");
pub const INJECTIONS_QUERY: &str = "";
pub const LOCALS_QUERY: &str = "";
pub const TAGS_QUERY: &str = "";

#[cfg(test)]
mod tests {
    #[test]
    fn test_can_load_grammar() {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::language())
            .expect("Error loading ptx language");
    }
}