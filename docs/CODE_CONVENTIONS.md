# Code conventions used in this project

## Code style

1. In headers, there is always `#pragma once` first and a newline after it.
2. Includes are split into 5 categories: the corresponding header (for source files only), the relative includes (written in ""), other project includes (written with <>), third-party includes, standard library includes. Within each category includes are sorted lexicographically, a newline must be written between categories.
3. In the end of the file, there is always a newline.
4. Nested namespaces are written at once (like `namespace a::b { ... } // namespace a::b`). There is a newline after namespace declaration and before the closing `}`. No indentation is added to namespace contents. In source files for file-local functions and global declarations, anonymous namespace is used.
5. When a declaration or expression does not fit (the limit is 120 characters, in extreme cases 132 is allowed), they are wrapped so that the additional contents have only one additional indentation. The preceding operators are wrapped. Some examples (they might fit in fact, just imagine they are way longer):
```
class A
    : public B
    , C
{ // The rare case when a standalone { is allowed - mostly it is prohibited.
    A(int x)
        : m_x(x)
    { }

    void some_method(
        int a,
        char const b
    ) override;
}

void Class::some_method(
    int a,
    char const b
) {
    // code
}

bool const result = true
    && expr1
    && expr2
;
// Or if additional operand is not free or just doesn't exist:
Value v = expr1
    << expr 2
    << expr3
;
```
6. If comments are longer than 3 lines, they are written like
```
/*
 * One
 * Two
 * Three
**/
```
7. Single-line ifs, fors, whiles, etc always have braces and the statement is written on a separate line.
8. Attributes like `[[nodiscard]]` are written on a separate line. But if they come in a macro, then they are written on the same line. The return type is never written on a separate line.
9. `const` is always written to the right: `char const* const name`.
10. Naming conventions: `MACRO`, `namespace_name`, `SomeType`, `someFunctionOrMethod`, `some_variable_or_parameter`, `SOME_CONSTANT`, `EnumValue`. Also, there are specific cases: `IInterfaceType`, `g_global_variable`, `s_static_variable_or_field`, `m_private_or_protected_field`.
11. `struct` types are only those that have no private nor protected modifiers - they are fully public. Otherwise it is a class. `typename` must be used instead of `class` where applicable.
12. Keep all entities atomic and modular, with clean contracts. Avoid long functions and classes. Do not place multiple entities in one header. Adhere to clear separation of concerns - code entities must not have multiple responsibilities. Also, there must always be a clear contract. `Util` is a bad name - it tells nothing about the entity. `value` is a bad name unless we have a single-line setter. `str` is a bad name. Etc.
13. Avoid deep scope nesting. Prefer early returns, continues, breaks. Instead of `if (a) { ... }` prefer `if (!a) { continue/break/return } ...`.
14. For atomics, always use `load` and `store` explicitly.
15. Always use `using` instead of `typedef`.
16. Keep single-line (or two-line) functions in headers, move others to the source file if possible.
17. Use designated initializers when possible.
18. Within a file, the order of declarations must be: includes -> type aliases -> global constants -> global variables -> anonymous namespace if any -> type declarations -> free-standing function declarations.
19. Within a class, the order of declarations must be: type aliases -> nested types (if possible only declared, with actual implementations later) -> public fields -> constructors -> destructor -> assignment operators -> public static methods -> operator== -> operator<=> -> operator() -> operator[] -> other operators -> public methods -> protected static methods -> protected methods -> private static methods -> private methods -> non-public fields. If the class has large method bodies that cannot be moved to the source file (e.g. for a template class), then they must be moved out to be after the class.
20. For numbers with 4 digits and more use the `'` separator.
21. Use 4-space indentation.
22. For logging, use `Log.hpp`. Never use std::cout / cerr / clog directly to print messages.
23. Trailing comma must be used in multiline lists when possible (in enums, arrays, brace initialization).

## Other conventions

1. The stricter the code the better. If possible and justified, `[[nodiscard]]`, `constexpr`, `noexcept`, `const`, `final` and others must be added. Where applicable, template parameters must be explicitly constrained.
2. C-types of unknown size must not be used in favor of `uint32_t`, `size_t`, `ptrdiff_t`. Those are written without `std::` prefix.
3. Raw pointers must be avoided wherever possible. Use references, smart pointers, `std::array`, `std::vector`, `std::span`, etc.
4. If the required memory is known in advance or easily calculated, use `reserve`. When justified, use `pmr::` containers and polymorphic allocators.
5. Always explicitly state the pre-contract using `ASSERT` and `HIGH_ASSERT`. The former is checked always, the latter is only for debug mode. You can also use `ASSUME` which adds assert in debug mode and only a hint to compiler in release. So in hot paths or for heavy checks use `ASSUME` or `HIGH_ASSERT`, for fast or rare checks use `ASSERT`.
6. Always handle any possible errors - at least print a log message. For truly rare errors use exceptions, for those with expectedly high frequence (more than 1% of all cases) used `std::expected` or error codes. Handle exceptions at crucial points, try to recover or reload when possible.
7. Excessive comments must be avoided. The entity names must be chosen so that the meaning is clean without additional explanation. Function contract must be clear from its name, arguments, and return type. Add comments only where absolutely necessary, e.g. to explain how some algorithm works, to mark some unfinished work (use `// TODO: `), and in the cases where it is hard to perceive the code otherwise.
8. Prefer `enum class` to `enum`.
9. Avoid `auto` overuse. `auto` is good in complex template contexts or when the type is a very long name. But otherwise, state the type explicitly.
10. Do not use strings where `enum` is enough.
