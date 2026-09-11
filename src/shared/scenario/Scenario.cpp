#include <shared/scenario/Scenario.hpp>

#include <limits>
#include <utility>

namespace shared {

namespace scenario_detail {

static constexpr uint8_t MAX_POSITION = 31;

enum class ScenarioTokenKind : uint8_t {
    Word,
    String,
};

struct ScenarioToken final {
    ScenarioTokenKind kind;
    std::string text;
    ScenarioLocation location;
};

struct ScenarioLine final {
    std::vector<ScenarioToken> tokens;
    ScenarioLocation location;
};

[[nodiscard]]
bool isAsciiLetter(char const ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

[[nodiscard]]
bool isAsciiDigit(char const ch) noexcept {
    return ch >= '0' && ch <= '9';
}

[[nodiscard]]
bool isWordCharacter(char const ch) noexcept {
    return isAsciiLetter(ch) || isAsciiDigit(ch) || ch == '_' || ch == '-' || ch == '+';
}

[[nodiscard]]
bool isIdentifier(std::string_view const text) noexcept {
    if (text.empty() || (!isAsciiLetter(text.front()) && text.front() != '_')) {
        return false;
    }
    for (char const ch : text.substr(1)) {
        if (!isAsciiLetter(ch) && !isAsciiDigit(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
bool isAllowedCharacter(char const character) noexcept {
    return character == '@' || character == '#' || character == '$' || character == '%' || character == '&';
}

[[nodiscard]]
bool actorPlacementsConflict(
    ScenarioActor const& actor,
    uint8_t const x,
    uint8_t const y
) noexcept {
    uint32_t const actor_x = actor.x;
    uint32_t const actor_y = actor.y;
    uint32_t const candidate_x = x;
    uint32_t const candidate_y = y;
    uint32_t const delta_x = actor_x > candidate_x ? actor_x - candidate_x : candidate_x - actor_x;
    uint32_t const delta_y = actor_y > candidate_y ? actor_y - candidate_y : candidate_y - actor_y;
    return delta_x <= 1 && delta_y <= 1;
}

[[nodiscard]]
ScenarioDiagnostic makeDiagnostic(
    ScenarioDiagnosticCode const code,
    std::string_view const filename,
    ScenarioLocation const location,
    std::string message
) {
    return ScenarioDiagnostic{
        .code = code,
        .filename = std::string{ filename },
        .location = location,
        .message = std::move(message),
    };
}

[[nodiscard]]
std::expected<std::vector<ScenarioLine>, ScenarioDiagnostic> lexScenario(
    std::string_view const filename,
    std::string_view const source
) {
    std::vector<ScenarioLine> lines;
    ScenarioLine line{ .tokens = {}, .location = { .line = 1, .column = 1 }};
    uint32_t current_line = 1;
    uint32_t current_column = 1;
    uint64_t index = 0;

    auto finishLine = [&lines, &line]() {
        if (!line.tokens.empty()) {
            line.location = line.tokens.front().location;
            lines.push_back(std::move(line));
            line = ScenarioLine{ .tokens = {}, .location = { .line = 0, .column = 0 }};
        }
    };
    auto advanceLine = [&current_line, &current_column]() {
        ++current_line;
        current_column = 1;
    };

    while (index < source.size()) {
        char const character = source[index];
        if (character == '\r') {
            if (index + 1 < source.size() && source[index + 1] == '\n') {
                ++index;
            }
            ++index;
            finishLine();
            advanceLine();
            continue;
        }
        if (character == '\n') {
            ++index;
            finishLine();
            advanceLine();
            continue;
        }
        if (character == ' ' || character == '\t') {
            ++index;
            ++current_column;
            continue;
        }
        if (character == '#') {
            while (index < source.size() && source[index] != '\r' && source[index] != '\n') {
                ++index;
                ++current_column;
            }
            continue;
        }

        ScenarioLocation const location{ .line = current_line, .column = current_column };
        if (character == '"') {
            ++index;
            ++current_column;
            std::string text;
            bool closed = false;
            while (index < source.size()) {
                char const string_character = source[index];
                if (string_character == '"') {
                    ++index;
                    ++current_column;
                    closed = true;
                    break;
                }
                if (string_character == '\r' || string_character == '\n') {
                    return std::unexpected(makeDiagnostic(
                        ScenarioDiagnosticCode::MalformedSyntax,
                        filename,
                        location,
                        "quoted strings cannot span lines"
                    ));
                }
                if (static_cast<unsigned char>(string_character) > 127) {
                    return std::unexpected(makeDiagnostic(
                        ScenarioDiagnosticCode::MalformedSyntax,
                        filename,
                        { .line = current_line, .column = current_column },
                        "scenario text must be ASCII"
                    ));
                }
                if (string_character != '\\') {
                    text.push_back(string_character);
                    ++index;
                    ++current_column;
                    continue;
                }

                ++index;
                ++current_column;
                if (index == source.size()) {
                    return std::unexpected(makeDiagnostic(
                        ScenarioDiagnosticCode::MalformedSyntax,
                        filename,
                        location,
                        "quoted string has an incomplete escape"
                    ));
                }
                char const escaped = source[index];
                char replacement;
                switch (escaped) {
                    case '"': replacement = '"'; break;
                    case '\\': replacement = '\\'; break;
                    case 'n': replacement = '\n'; break;
                    case 'r': replacement = '\r'; break;
                    case 't': replacement = '\t'; break;
                    default: {
                        return std::unexpected(makeDiagnostic(
                            ScenarioDiagnosticCode::MalformedSyntax,
                            filename,
                            { .line = current_line, .column = current_column },
                            "quoted string has an unsupported escape"
                        ));
                    }
                }
                text.push_back(replacement);
                ++index;
                ++current_column;
            }
            if (!closed) {
                return std::unexpected(makeDiagnostic(
                    ScenarioDiagnosticCode::MalformedSyntax,
                    filename,
                    location,
                    "quoted string is not closed"
                ));
            }
            line.tokens.push_back(ScenarioToken{
                .kind = ScenarioTokenKind::String,
                .text = std::move(text),
                .location = location,
            });
            continue;
        }
        if (!isWordCharacter(character)) {
            return std::unexpected(makeDiagnostic(
                ScenarioDiagnosticCode::MalformedSyntax,
                filename,
                location,
                "unexpected character"
            ));
        }

        uint64_t const start = index;
        while (index < source.size() && isWordCharacter(source[index])) {
            ++index;
            ++current_column;
        }
        line.tokens.push_back(ScenarioToken{
            .kind = ScenarioTokenKind::Word,
            .text = std::string{ source.substr(start, index - start) },
            .location = location,
        });
    }
    finishLine();
    return lines;
}

class ScenarioParser final {
public:
    ScenarioParser(
        std::string_view const filename,
        ScenarioLimits const& limits,
        std::vector<ScenarioLine> lines,
        ScenarioLocation const end_location
    )
        : m_filename(filename)
        , m_limits(limits)
        , m_lines(std::move(lines))
        , m_end_location(end_location) {}

    [[nodiscard]]
    std::expected<ScenarioPlan, ScenarioDiagnostic> parse() {
        if (auto const valid = validateLimits(); !valid) {
            return std::unexpected(valid.error());
        }
        if (auto const header = parseHeader(); !header) {
            return std::unexpected(header.error());
        }
        if (auto const profile = parseProfile(); !profile) {
            return std::unexpected(profile.error());
        }
        if (auto const seed = parseSeed(); !seed) {
            return std::unexpected(seed.error());
        }
        if (auto const actors = parseActors(); !actors) {
            return std::unexpected(actors.error());
        }
        if (auto const begin = parseBegin(); !begin) {
            return std::unexpected(begin.error());
        }
        if (auto const body = parseBody(); !body) {
            return std::unexpected(body.error());
        }

        return ScenarioPlan{
            m_version,
            m_profile,
            m_seed,
            std::move(m_actors),
            std::move(m_operations),
            m_total_ticks,
            m_evidence_count,
        };
    }

private:
    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> validateLimits() const {
        if (m_limits.max_source_bytes == 0
            || m_limits.max_statements == 0
            || m_limits.max_actors == 0
            || m_limits.max_total_ticks == 0
            || m_limits.max_operations == 0
            || m_limits.max_evidence == 0) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidLimits,
                { .line = 1, .column = 1 },
                "all scenario limits must be positive"
            ));
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseHeader() {
        ScenarioLine const* const line = nextLine();
        if (!line) {
            return std::unexpected(expectedLine("scenario header"));
        }
        if (auto const count = expectStatement(*line); !count) {
            return std::unexpected(count.error());
        }
        if (!hasExactWords(*line, "scenario", 2)) {
            return std::unexpected(malformed(*line, "expected 'scenario <version>'"));
        }
        auto const version = parseUnsigned(line->tokens[1]);
        if (!version) {
            return std::unexpected(version.error());
        }
        if (*version > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::IntegerOverflow,
                line->tokens[1].location,
                "scenario version overflows uint32"
            ));
        }
        if (*version != 1) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::UnsupportedVersion,
                line->tokens[1].location,
                "only scenario version 1 is supported"
            ));
        }
        m_version = static_cast<uint32_t>(*version);
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseProfile() {
        ScenarioLine const* const line = nextLine();
        if (!line) {
            return std::unexpected(expectedLine("profile"));
        }
        if (auto const count = expectStatement(*line); !count) {
            return std::unexpected(count.error());
        }
        if (!hasExactWords(*line, "profile", 2)) {
            return std::unexpected(malformed(*line, "expected 'profile flat2d-v1'"));
        }
        if (line->tokens[1].kind != ScenarioTokenKind::Word) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::MalformedSyntax,
                line->tokens[1].location,
                "profile name must be an unquoted ASCII identifier"
            ));
        }
        if (line->tokens[1].text != "flat2d-v1") {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::UnsupportedProfile,
                line->tokens[1].location,
                "only profile flat2d-v1 is supported"
            ));
        }
        m_profile = ScenarioProfile::Flat2dV1;
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseSeed() {
        ScenarioLine const* const line = nextLine();
        if (!line) {
            return std::unexpected(expectedLine("seed"));
        }
        if (auto const count = expectStatement(*line); !count) {
            return std::unexpected(count.error());
        }
        if (!hasExactWords(*line, "seed", 2)) {
            return std::unexpected(malformed(*line, "expected 'seed <unsigned-integer>'"));
        }
        auto const seed = parseUnsigned(line->tokens[1]);
        if (!seed) {
            return std::unexpected(seed.error());
        }
        m_seed = *seed;
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseActors() {
        while (m_line_index < m_lines.size() && firstWord(m_lines[m_line_index]) == "player") {
            ScenarioLine const* const line = nextLine();
            if (auto const count = expectStatement(*line); !count) {
                return std::unexpected(count.error());
            }
            if (line->tokens.size() != 7 || !hasWord(*line, 2, "character")
                || line->tokens[3].kind != ScenarioTokenKind::String || !hasWord(*line, 4, "at")) {
                return std::unexpected(malformed(*line, "expected 'player <name> character <char> at <x> <y>'"));
            }
            if (line->tokens[1].kind != ScenarioTokenKind::Word || !isIdentifier(line->tokens[1].text)) {
                return std::unexpected(diagnostic(
                    ScenarioDiagnosticCode::MalformedSyntax,
                    line->tokens[1].location,
                    "player name must be an ASCII identifier"
                ));
            }
            if (line->tokens[3].text.size() != 1 || !isAllowedCharacter(line->tokens[3].text.front())) {
                return std::unexpected(diagnostic(
                    ScenarioDiagnosticCode::InvalidCharacter,
                    line->tokens[3].location,
                    "player character must be one of @ # $ % &"
                ));
            }
            auto const x = parsePosition(line->tokens[5]);
            if (!x) {
                return std::unexpected(x.error());
            }
            auto const y = parsePosition(line->tokens[6]);
            if (!y) {
                return std::unexpected(y.error());
            }
            if (m_actors.size() >= m_limits.max_actors) {
                return std::unexpected(diagnostic(
                    ScenarioDiagnosticCode::ActorLimitExceeded,
                    line->location,
                    "scenario actor limit exceeded"
                ));
            }
            for (ScenarioActor const& actor : m_actors) {
                if (actor.name == line->tokens[1].text) {
                    return std::unexpected(diagnostic(
                        ScenarioDiagnosticCode::DuplicateActor,
                        line->tokens[1].location,
                        "player name must be unique"
                    ));
                }
                if (actor.character == line->tokens[3].text.front()) {
                    return std::unexpected(diagnostic(
                        ScenarioDiagnosticCode::DuplicateActor,
                        line->tokens[3].location,
                        "player character must be unique"
                    ));
                }
                if (actorPlacementsConflict(actor, *x, *y)) {
                    return std::unexpected(diagnostic(
                        ScenarioDiagnosticCode::InvalidRange,
                        line->location,
                        "player '" + line->tokens[1].text + "' at " + std::to_string(*x) + " "
                            + std::to_string(*y) + " conflicts with player '" + actor.name + "' at "
                            + std::to_string(actor.x) + " " + std::to_string(actor.y)
                    ));
                }
            }
            m_actors.push_back(ScenarioActor{
                .id = static_cast<ScenarioActorId>(m_actors.size()),
                .name = line->tokens[1].text,
                .character = line->tokens[3].text.front(),
                .x = *x,
                .y = *y,
                .location = line->location,
            });
        }
        if (m_actors.empty()) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::MissingPlayer,
                currentLocation(),
                "scenario must declare at least one player"
            ));
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseBegin() {
        ScenarioLine const* const line = nextLine();
        if (!line) {
            return std::unexpected(expectedLine("begin"));
        }
        if (auto const count = expectStatement(*line); !count) {
            return std::unexpected(count.error());
        }
        if (!hasExactWords(*line, "begin", 1)) {
            return std::unexpected(malformed(*line, "expected 'begin'"));
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseBody() {
        while (m_line_index < m_lines.size()) {
            ScenarioLine const* const line = nextLine();
            if (auto const count = expectStatement(*line); !count) {
                return std::unexpected(count.error());
            }
            std::string const& command = firstWord(*line);
            if (command == "end") {
                if (!hasExactWords(*line, "end", 1)) {
                    return std::unexpected(malformed(*line, "expected 'end'"));
                }
                if (m_line_index != m_lines.size()) {
                    return std::unexpected(malformed(m_lines[m_line_index], "unexpected text after 'end'"));
                }
                return {};
            }
            if (command == "input") {
                if (auto const input = parseInput(*line); !input) {
                    return std::unexpected(input.error());
                }
                continue;
            }
            if (command == "wait") {
                if (auto const wait = parseWait(*line); !wait) {
                    return std::unexpected(wait.error());
                }
                continue;
            }
            if (command == "expect") {
                if (auto const expect = parseExpectation(*line); !expect) {
                    return std::unexpected(expect.error());
                }
                continue;
            }
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::UnsupportedCommand,
                line->location,
                "unsupported scenario command"
            ));
        }
        return std::unexpected(expectedLine("end"));
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseInput(ScenarioLine const& line) {
        if (line.tokens.size() != 4) {
            return std::unexpected(malformed(line, "expected 'input <player> <x> <y>'"));
        }
        auto const actor = actorId(line.tokens[1]);
        if (!actor) {
            return std::unexpected(actor.error());
        }
        auto const x = parseInputComponent(line.tokens[2]);
        if (!x) {
            return std::unexpected(x.error());
        }
        auto const y = parseInputComponent(line.tokens[3]);
        if (!y) {
            return std::unexpected(y.error());
        }
        if (m_boundary == std::numeric_limits<uint64_t>::max()) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::IntegerOverflow,
                line.location,
                "input effective boundary overflows uint64"
            ));
        }
        return addOperation(ScenarioOperation{
            .location = line.location,
            .boundary = m_boundary,
            .data = ScenarioInputOperation{
                .actor = *actor,
                .x = *x,
                .y = *y,
                .effective_boundary = m_boundary + 1,
            },
        });
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseWait(ScenarioLine const& line) {
        if (line.tokens.size() != 2) {
            return std::unexpected(malformed(line, "expected 'wait <positive-integer>'"));
        }
        if (line.tokens[1].kind != ScenarioTokenKind::Word
            || (line.tokens[1].text.size() > 1 && line.tokens[1].text.front() == '0')) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidInteger,
                line.tokens[1].location,
                "wait ticks must use canonical base-10 notation without leading zeroes"
            ));
        }
        auto const ticks = parseUnsigned(line.tokens[1]);
        if (!ticks) {
            return std::unexpected(ticks.error());
        }
        if (*ticks == 0) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidRange,
                line.tokens[1].location,
                "wait ticks must be positive"
            ));
        }
        if (*ticks > std::numeric_limits<uint64_t>::max() - m_total_ticks) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::IntegerOverflow,
                line.tokens[1].location,
                "total scenario ticks overflow uint64"
            ));
        }
        uint64_t const next_total_ticks = m_total_ticks + *ticks;
        if (next_total_ticks > m_limits.max_total_ticks) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::TickLimitExceeded,
                line.location,
                "scenario tick limit exceeded"
            ));
        }
        auto const operation = ScenarioOperation{
            .location = line.location,
            .boundary = m_boundary,
            .data = ScenarioWaitOperation{ .ticks = *ticks },
        };
        if (auto const added = addOperation(operation); !added) {
            return std::unexpected(added.error());
        }
        m_total_ticks = next_total_ticks;
        m_boundary = next_total_ticks;
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> parseExpectation(ScenarioLine const& line) {
        if (line.tokens.size() != 6 || !hasWord(line, 1, "player") || !hasWord(line, 3, "position")) {
            return std::unexpected(malformed(line, "expected 'expect player <name> position <x> <y>'"));
        }
        auto const actor = actorId(line.tokens[2]);
        if (!actor) {
            return std::unexpected(actor.error());
        }
        auto const x = parsePosition(line.tokens[4]);
        if (!x) {
            return std::unexpected(x.error());
        }
        auto const y = parsePosition(line.tokens[5]);
        if (!y) {
            return std::unexpected(y.error());
        }
        if (m_evidence_count == m_limits.max_evidence) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::EvidenceLimitExceeded,
                line.location,
                "scenario evidence limit exceeded"
            ));
        }
        auto const operation = ScenarioOperation{
            .location = line.location,
            .boundary = m_boundary,
            .data = ScenarioExpectPositionOperation{
                .actor = *actor,
                .x = *x,
                .y = *y,
            },
        };
        if (auto const added = addOperation(operation); !added) {
            return std::unexpected(added.error());
        }
        ++m_evidence_count;
        return {};
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> addOperation(ScenarioOperation operation) {
        if (m_operations.size() >= m_limits.max_operations) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::OperationLimitExceeded,
                operation.location,
                "scenario operation limit exceeded"
            ));
        }
        m_operations.push_back(std::move(operation));
        return {};
    }

    [[nodiscard]]
    std::expected<uint64_t, ScenarioDiagnostic> parseUnsigned(ScenarioToken const& token) const {
        if (token.kind != ScenarioTokenKind::Word || token.text.empty()) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidInteger,
                token.location,
                "expected a base-10 integer"
            ));
        }
        uint64_t result = 0;
        for (char const character : token.text) {
            if (!isAsciiDigit(character)) {
                return std::unexpected(diagnostic(
                    ScenarioDiagnosticCode::InvalidInteger,
                    token.location,
                    "expected a base-10 integer"
                ));
            }
            uint64_t const digit = static_cast<uint64_t>(character - '0');
            if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                return std::unexpected(diagnostic(
                    ScenarioDiagnosticCode::IntegerOverflow,
                    token.location,
                    "base-10 integer overflows uint64"
                ));
            }
            result = result * 10 + digit;
        }
        return result;
    }

    [[nodiscard]]
    std::expected<int8_t, ScenarioDiagnostic> parseInputComponent(ScenarioToken const& token) const {
        if (token.kind != ScenarioTokenKind::Word || token.text.empty()) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidInteger,
                token.location,
                "expected a base-10 integer"
            ));
        }
        bool const negative = token.text.front() == '-';
        std::string_view const digits = negative ? std::string_view{ token.text }.substr(1) : token.text;
        ScenarioToken const unsigned_token{
            .kind = token.kind,
            .text = std::string{ digits },
            .location = token.location,
        };
        auto const magnitude = parseUnsigned(unsigned_token);
        if (!magnitude) {
            return std::unexpected(magnitude.error());
        }
        if (*magnitude > 1) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidRange,
                token.location,
                "input component must be in -1..1"
            ));
        }
        int8_t const input_component = static_cast<int8_t>(*magnitude);
        if (negative) {
            return static_cast<int8_t>(-input_component);
        }
        return input_component;
    }

    [[nodiscard]]
    std::expected<uint8_t, ScenarioDiagnostic> parsePosition(ScenarioToken const& token) const {
        if (token.kind == ScenarioTokenKind::Word && token.text.size() > 1 && token.text.front() == '-') {
            ScenarioToken const magnitude_token{
                .kind = token.kind,
                .text = token.text.substr(1),
                .location = token.location,
            };
            if (auto const magnitude = parseUnsigned(magnitude_token); !magnitude) {
                return std::unexpected(magnitude.error());
            }
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidRange,
                token.location,
                "position must be in 0..31"
            ));
        }
        auto const value = parseUnsigned(token);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (*value > MAX_POSITION) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::InvalidRange,
                token.location,
                "position must be in 0..31"
            ));
        }
        return static_cast<uint8_t>(*value);
    }

    [[nodiscard]]
    std::expected<ScenarioActorId, ScenarioDiagnostic> actorId(ScenarioToken const& token) const {
        if (token.kind != ScenarioTokenKind::Word || !isIdentifier(token.text)) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::MalformedSyntax,
                token.location,
                "player name must be an ASCII identifier"
            ));
        }
        for (ScenarioActor const& actor : m_actors) {
            if (actor.name == token.text) {
                return actor.id;
            }
        }
        return std::unexpected(diagnostic(
            ScenarioDiagnosticCode::UnknownActor,
            token.location,
            "scenario command refers to an unknown player"
        ));
    }

    [[nodiscard]]
    std::expected<void, ScenarioDiagnostic> expectStatement(ScenarioLine const& line) {
        if (m_statement_count == m_limits.max_statements) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::StatementLimitExceeded,
                line.location,
                "scenario statement limit exceeded"
            ));
        }
        ++m_statement_count;
        return {};
    }

    [[nodiscard]]
    ScenarioLine const* nextLine() {
        if (m_line_index == m_lines.size()) {
            return nullptr;
        }
        return &m_lines[m_line_index++];
    }

    [[nodiscard]]
    std::string const& firstWord(ScenarioLine const& line) const {
        static std::string const EMPTY;
        if (line.tokens.empty() || line.tokens.front().kind != ScenarioTokenKind::Word) {
            return EMPTY;
        }
        return line.tokens.front().text;
    }

    [[nodiscard]]
    bool hasWord(ScenarioLine const& line, uint64_t const index, std::string_view const expected) const {
        return index < line.tokens.size()
            && line.tokens[index].kind == ScenarioTokenKind::Word
            && line.tokens[index].text == expected;
    }

    [[nodiscard]]
    bool hasExactWords(ScenarioLine const& line, std::string_view const first, uint64_t const count) const {
        return line.tokens.size() == count && hasWord(line, 0, first);
    }

    [[nodiscard]]
    ScenarioDiagnostic malformed(ScenarioLine const& line, std::string message) const {
        return diagnostic(ScenarioDiagnosticCode::MalformedSyntax, line.location, std::move(message));
    }

    [[nodiscard]]
    ScenarioDiagnostic expectedLine(std::string_view const expected) const {
        return diagnostic(
            ScenarioDiagnosticCode::MalformedSyntax,
            m_end_location,
            "expected " + std::string{ expected } + " before end of file"
        );
    }

    [[nodiscard]]
    ScenarioLocation currentLocation() const {
        if (m_line_index < m_lines.size()) {
            return m_lines[m_line_index].location;
        }
        return m_end_location;
    }

    [[nodiscard]]
    ScenarioDiagnostic diagnostic(
        ScenarioDiagnosticCode const code,
        ScenarioLocation const location,
        std::string message
    ) const {
        return makeDiagnostic(code, m_filename, location, std::move(message));
    }

private:
    std::string_view m_filename;
    ScenarioLimits const& m_limits;
    std::vector<ScenarioLine> m_lines;
    ScenarioLocation m_end_location;
    uint64_t m_line_index = 0;
    uint64_t m_statement_count = 0;
    uint32_t m_version = 0;
    ScenarioProfile m_profile = ScenarioProfile::Flat2dV1;
    uint64_t m_seed = 0;
    std::vector<ScenarioActor> m_actors;
    std::vector<ScenarioOperation> m_operations;
    uint64_t m_boundary = 0;
    uint64_t m_total_ticks = 0;
    uint64_t m_evidence_count = 0;
};

} // namespace scenario_detail

std::string_view scenarioProfileName(ScenarioProfile const profile) noexcept {
    switch (profile) {
        case ScenarioProfile::Flat2dV1: return "flat2d-v1";
    }
    return "unknown";
}

std::string_view scenarioDiagnosticCodeName(ScenarioDiagnosticCode const code) noexcept {
    switch (code) {
        case ScenarioDiagnosticCode::InvalidLimits: return "invalid-limits";
        case ScenarioDiagnosticCode::SourceTooLarge: return "source-too-large";
        case ScenarioDiagnosticCode::StatementLimitExceeded: return "statement-limit-exceeded";
        case ScenarioDiagnosticCode::ActorLimitExceeded: return "actor-limit-exceeded";
        case ScenarioDiagnosticCode::TickLimitExceeded: return "tick-limit-exceeded";
        case ScenarioDiagnosticCode::OperationLimitExceeded: return "operation-limit-exceeded";
        case ScenarioDiagnosticCode::EvidenceLimitExceeded: return "evidence-limit-exceeded";
        case ScenarioDiagnosticCode::MalformedSyntax: return "malformed-syntax";
        case ScenarioDiagnosticCode::UnsupportedVersion: return "unsupported-version";
        case ScenarioDiagnosticCode::UnsupportedProfile: return "unsupported-profile";
        case ScenarioDiagnosticCode::DuplicateActor: return "duplicate-actor";
        case ScenarioDiagnosticCode::UnknownActor: return "unknown-actor";
        case ScenarioDiagnosticCode::UnsupportedCommand: return "unsupported-command";
        case ScenarioDiagnosticCode::IntegerOverflow: return "integer-overflow";
        case ScenarioDiagnosticCode::InvalidInteger: return "invalid-integer";
        case ScenarioDiagnosticCode::InvalidRange: return "invalid-range";
        case ScenarioDiagnosticCode::InvalidCharacter: return "invalid-character";
        case ScenarioDiagnosticCode::MissingPlayer: return "missing-player";
        case ScenarioDiagnosticCode::UnknownSourceHeader: return "unknown-source-header";
        case ScenarioDiagnosticCode::CoreLangCompileFailure: return "corelang-compile-failure";
        case ScenarioDiagnosticCode::CoreLangRuntimeFailure: return "corelang-runtime-failure";
        case ScenarioDiagnosticCode::Cancelled: return "cancelled";
    }
    return "unknown";
}

ScenarioPlan::ScenarioPlan(
    uint32_t const version,
    ScenarioProfile const profile,
    uint64_t const seed,
    std::vector<ScenarioActor> actors,
    std::vector<ScenarioOperation> operations,
    uint64_t const total_ticks,
    uint64_t const evidence_count
)
    : m_version(version)
    , m_profile(profile)
    , m_seed(seed)
    , m_actors(std::move(actors))
    , m_operations(std::move(operations))
    , m_total_ticks(total_ticks)
    , m_evidence_count(evidence_count) {}

uint32_t ScenarioPlan::version() const noexcept {
    return m_version;
}

ScenarioProfile ScenarioPlan::profile() const noexcept {
    return m_profile;
}

uint64_t ScenarioPlan::seed() const noexcept {
    return m_seed;
}

std::vector<ScenarioActor> const& ScenarioPlan::actors() const noexcept {
    return m_actors;
}

std::vector<ScenarioOperation> const& ScenarioPlan::operations() const noexcept {
    return m_operations;
}

uint64_t ScenarioPlan::totalTicks() const noexcept {
    return m_total_ticks;
}

uint64_t ScenarioPlan::evidenceCount() const noexcept {
    return m_evidence_count;
}

std::expected<ScenarioPlan, ScenarioDiagnostic> parseScenario(
    std::string_view const filename,
    std::string_view const source,
    ScenarioLimits const& limits
) {
    if (limits.max_source_bytes == 0
        || limits.max_statements == 0
        || limits.max_actors == 0
        || limits.max_total_ticks == 0
        || limits.max_operations == 0
        || limits.max_evidence == 0) {
        return std::unexpected(scenario_detail::makeDiagnostic(
            ScenarioDiagnosticCode::InvalidLimits,
            filename,
            { .line = 1, .column = 1 },
            "all scenario limits must be positive"
        ));
    }
    if (source.size() > limits.max_source_bytes) {
        return std::unexpected(scenario_detail::makeDiagnostic(
            ScenarioDiagnosticCode::SourceTooLarge,
            filename,
            { .line = 1, .column = 1 },
            "scenario source exceeds the configured byte limit"
        ));
    }
    auto const lines = scenario_detail::lexScenario(filename, source);
    if (!lines) {
        return std::unexpected(lines.error());
    }

    uint32_t end_line = 1;
    uint32_t end_column = 1;
    uint64_t source_index = 0;
    while (source_index < source.size()) {
        char const character = source[source_index++];
        if (character == '\r') {
            if (source_index < source.size() && source[source_index] == '\n') {
                ++source_index;
            }
            ++end_line;
            end_column = 1;
        } else if (character == '\n') {
            ++end_line;
            end_column = 1;
        } else {
            ++end_column;
        }
    }
    return scenario_detail::ScenarioParser{
        filename,
        limits,
        *lines,
        { .line = end_line, .column = end_column },
    }.parse();
}

} // namespace shared
