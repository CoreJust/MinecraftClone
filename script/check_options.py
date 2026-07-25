SRC_SUBFOLDERS = ('client/', 'server/', 'shared/', 'core/')
CODE_EXTS = { '.hpp', '.cpp' }
SHADER_EXTS = { '.mesh', '.task', '.vert', '.frag', '.comp' }

INCLUDES_ORDER = ('self', 'relative', 'project', 'thirdparty', 'std')
LINE_LENGTH_MAX = 132
MAX_NESTING_DEPTH = 7
MAX_DIGITS_WITHOUT_SEPARATOR = 4
FORBIDDEN_WORDS = {
    'typedef',
    'std::uint8_t', 'std::int8_t', 'std::uint16_t', 'std::int16_t',
    'std::uint32_t', 'std::int32_t', 'std::uint64_t', 'std::int64_t',
    'std::size_t', 'std::ptrdiff_t'
}

DONT_CHECKS = {
    'digit_separators': 'DIGIT_SEPARATORS',
    'include_order': 'INCLUDE_ORDER',
    'pragma_once': 'NO_PRAGMA_ONCE',
    'line_length': 'LINE_LENGTH'
}

STD_HEADERS = {
    'cstdlib', 'execution',
    'cfloat', 'climits', 'compare', 'contracts', 'coroutine',
    'csetjmp', 'csignal', 'cstdarg', 'cstddef', 'cstdint',
    'exception', 'initializer_list', 'limits', 'new', 'source_location',
    'stdfloat', 'typeindex', 'typeinfo', 'version',
    'concepts',
    'cassert', 'cerrno', 'debugging', 'stacktrace', 'stdexcept',
    'system_error',
    'memory', 'memory_resource', 'scoped_allocator',
    'meta', 'ratio', 'type_traits',
    'any', 'bit', 'bitset', 'expected', 'functional', 'optional',
    'tuple', 'utility', 'variant',
    'array', 'deque', 'flat_map', 'flat_set', 'forward_list',
    'hive', 'inplace_vector', 'list', 'map', 'mdspan', 'queue',
    'set', 'span', 'stack', 'unordered_map', 'unordered_set',
    'vector',
    'iterator',
    'generator', 'ranges',
    'algorithm', 'numeric',
    'cstring', 'string', 'string_view',
    'cctype', 'charconv', 'clocale', 'codecvt', 'cuchar',
    'cwchar', 'cwctype', 'format', 'locale', 'regex', 'text_encoding',
    'cfenv', 'cmath', 'complex', 'linalg', 'numbers', 'random',
    'simd', 'valarray',
    'chrono', 'ctime',
    'cinttypes', 'cstdio', 'filesystem', 'fstream', 'iomanip', 'ios',
    'iostream', 'istream', 'ostream', 'print', 'spanstream', 'sstream',
    'streambuf', 'strstream', 'syncstream',
    'atomic', 'barrier', 'condition_variable', 'future', 'hazard_pointer',
    'latch', 'mutex', 'rcu', 'semaphore', 'shared_mutex', 'stop_token',
    'thread',
    'ccomplex', 'ctgmath'
}
