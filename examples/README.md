# Examples

Each program here is built and run by `make examples` (and by `make test`), so
an example that stops working fails the build. Run them with the same
`PREFIX=` the library was built with. They are C and use only the umbrella
header, `<ghoti.io/lang-tang/lang-tang.h>`.

| To see how to... | Read |
| --- | --- |
| Parse a template into a tree and count its nodes | [`parse_template.c`](parse_template.c), `main` |
| Parse a script instead of a template | [`parse_template.c`](parse_template.c), the second `gltang_parse` call (`GLTANG_PARSE_SCRIPT`) |
| Find out where and why a source was refused (`GLTANG_ParseError`) | [`parse_template.c`](parse_template.c), the second `gltang_parse` call |
| Release a tree | [`parse_template.c`](parse_template.c), `gltang_tree_destroy` |

The `tang` command is the same interface behind a command line: `tang FILE`
parses a template and prints its tree, `tang -s FILE` parses a script, and
`tang -e SOURCE` takes the source from the argument. `tang --help` lists the
rest. Running a template arrives with the interpreter.
