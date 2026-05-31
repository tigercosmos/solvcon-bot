#include "claude_stream_parser.hpp"

#include <modmesh/serialization/SerializableItem.hpp>

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace modmesh_bot
{

namespace
{

namespace mm = modmesh::detail;

const mm::JsonNode * find_node(const mm::JsonMap & obj, const std::string & key)
{
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return it->second.get();
}

// modmesh's parse_object stores a JsonType::String node's value as the
// raw expression INCLUDING the surrounding double quotes (e.g. the
// stored value for {"k":"v"} is the 3-byte string `"v"`). The public
// from_json_string<std::string> strips those quotes; reading node->value
// directly does not. Do the strip here, and decode the JSON escape
// sequences claude produces (\n, \t, \", \\, \uXXXX for BMP chars).
std::string decode_json_string(const std::string & quoted)
{
    if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"')
    {
        // Defensive: not actually a JSON string literal. Return as-is.
        return quoted;
    }
    std::string out;
    out.reserve(quoted.size());
    for (std::size_t i = 1; i + 1 < quoted.size(); ++i)
    {
        char c = quoted[i];
        if (c != '\\')
        {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= quoted.size()) break;
        const char esc = quoted[++i];
        switch (esc)
        {
        case '"':  out.push_back('"');  break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/');  break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u':
        {
            // \uXXXX — 4 hex digits, BMP code point. Emit UTF-8.
            if (i + 4 >= quoted.size()) return out;
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k)
            {
                const char hc = quoted[++i];
                cp <<= 4;
                if (hc >= '0' && hc <= '9') cp |= unsigned(hc - '0');
                else if (hc >= 'a' && hc <= 'f') cp |= unsigned(hc - 'a' + 10);
                else if (hc >= 'A' && hc <= 'F') cp |= unsigned(hc - 'A' + 10);
                else return out; // malformed
            }
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
        }
        default:
            // Unknown escape: pass through literally so we lose no info.
            out.push_back('\\');
            out.push_back(esc);
            break;
        }
    }
    return out;
}

std::optional<std::string> get_string(const mm::JsonMap & obj, const std::string & key)
{
    const auto * n = find_node(obj, key);
    if (!n || n->type != mm::JsonType::String) return std::nullopt;
    return decode_json_string(std::get<std::string>(n->value));
}

std::optional<double> get_number(const mm::JsonMap & obj, const std::string & key)
{
    const auto * n = find_node(obj, key);
    if (!n || n->type != mm::JsonType::Number) return std::nullopt;
    try
    {
        return std::stod(std::get<std::string>(n->value));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<bool> get_bool(const mm::JsonMap & obj, const std::string & key)
{
    const auto * n = find_node(obj, key);
    if (!n || n->type != mm::JsonType::Boolean) return std::nullopt;
    return std::get<std::string>(n->value) == "true";
}

const mm::JsonMap * get_object(const mm::JsonMap & obj, const std::string & key)
{
    const auto * n = find_node(obj, key);
    if (!n || n->type != mm::JsonType::Object) return nullptr;
    return &std::get<mm::JsonMap>(n->value);
}

const mm::JsonArray * get_array(const mm::JsonMap & obj, const std::string & key)
{
    const auto * n = find_node(obj, key);
    if (!n || n->type != mm::JsonType::Array) return nullptr;
    return &std::get<mm::JsonArray>(n->value);
}

// First N chars of `s` with newlines collapsed to spaces, suitable for
// one-line progress output. If `s` is longer than N, an ellipsis is
// appended.
std::string preview(const std::string & s, std::size_t n)
{
    std::string out = s.substr(0, n);
    for (char & c : out)
    {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (s.size() > n) out += "...";
    return out;
}

} // namespace

ClaudeStreamParser::ClaudeStreamParser() = default;
ClaudeStreamParser::~ClaudeStreamParser() = default;

void ClaudeStreamParser::set_progress_sink(ProgressSink sink)
{
    m_sink = std::move(sink);
}

void ClaudeStreamParser::feed(std::string_view chunk)
{
    m_partial_line.append(chunk.data(), chunk.size());

    std::size_t start = 0;
    while (true)
    {
        const auto pos = m_partial_line.find('\n', start);
        if (pos == std::string::npos) break;
        std::string_view line(m_partial_line.data() + start, pos - start);
        process_line(line);
        start = pos + 1;
    }
    if (start > 0)
    {
        m_partial_line.erase(0, start);
    }
}

void ClaudeStreamParser::flush()
{
    if (m_partial_line.empty()) return;
    std::string_view line(m_partial_line);
    process_line(line);
    m_partial_line.clear();
}

std::string ClaudeStreamParser::final_text() const
{
    if (m_saw_result && !m_final_result.empty()) return m_final_result;
    return m_assistant_text_fallback;
}

void ClaudeStreamParser::emit(std::string line)
{
    if (!m_sink) return;
    if (line.empty() || line.back() != '\n') line.push_back('\n');
    m_sink(line);
}

void ClaudeStreamParser::process_line(std::string_view raw)
{
    // Trim trailing CR/space and leading space. Claude emits LF-only
    // but be robust to terminal/Windows ports.
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' '
                            || raw.back() == '\t'))
    {
        raw.remove_suffix(1);
    }
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t'))
    {
        raw.remove_prefix(1);
    }
    if (raw.empty()) return;

    ++m_line_count;

    std::unique_ptr<mm::JsonNode> root;
    try
    {
        root = std::make_unique<mm::JsonNode>(
            mm::JsonType::Object, std::string(raw));
    }
    catch (const std::exception &)
    {
        ++m_parse_error_count;
        return;
    }

    if (root->type != mm::JsonType::Object)
    {
        ++m_parse_error_count;
        return;
    }
    const auto & obj = std::get<mm::JsonMap>(root->value);

    const std::string type = get_string(obj, "type").value_or("");

    if (type == "system")
    {
        const std::string subtype = get_string(obj, "subtype").value_or("");
        if (subtype == "init")
        {
            const std::string model = get_string(obj, "model").value_or("?");
            const std::string session = get_string(obj, "session_id").value_or("?");
            std::ostringstream oss;
            oss << "claude: init model=" << model
                << " session=" << session.substr(0, 8);
            emit(oss.str());
        }
        // Skip hook_started / hook_response / other system noise.
    }
    else if (type == "assistant")
    {
        ++m_assistant_turn_count;
        const auto * msg = get_object(obj, "message");
        if (msg == nullptr) return;
        const auto * content = get_array(*msg, "content");
        if (content == nullptr) return;

        // Walk content blocks. Most assistant turns have one text block;
        // tool-using turns interleave text + tool_use. We accumulate
        // text for the fallback and emit one progress line per block.
        for (const auto & elem : *content)
        {
            if (!elem || elem->type != mm::JsonType::Object) continue;
            const auto & e = std::get<mm::JsonMap>(elem->value);
            const std::string block_type = get_string(e, "type").value_or("");
            if (block_type == "text")
            {
                const std::string t = get_string(e, "text").value_or("");
                m_assistant_text_fallback.append(t);
                std::ostringstream oss;
                oss << "claude: turn " << m_assistant_turn_count
                    << " text(" << t.size() << "b): " << preview(t, 80);
                emit(oss.str());
            }
            else if (block_type == "tool_use")
            {
                const std::string name = get_string(e, "name").value_or("?");
                std::ostringstream oss;
                oss << "claude: turn " << m_assistant_turn_count
                    << " tool_use " << name;
                emit(oss.str());
            }
            else if (block_type == "thinking")
            {
                // Extended thinking blocks (when enabled). One-line
                // marker — we don't render the raw chain-of-thought to
                // stderr.
                const std::string t = get_string(e, "thinking").value_or("");
                std::ostringstream oss;
                oss << "claude: turn " << m_assistant_turn_count
                    << " thinking(" << t.size() << "b)";
                emit(oss.str());
            }
        }
    }
    else if (type == "user")
    {
        // Tool result turn (role=user, content=[{type:tool_result,...}]).
        // We don't need to dig into the result body; just mark it.
        emit("claude: tool result received");
    }
    else if (type == "result")
    {
        m_saw_result = true;
        m_final_result = get_string(obj, "result").value_or("");
        m_saw_error = get_bool(obj, "is_error").value_or(false);
        const double dur_ms = get_number(obj, "duration_ms").value_or(0.0);
        const double cost = get_number(obj, "total_cost_usd").value_or(0.0);
        const double turns = get_number(obj, "num_turns").value_or(0.0);
        std::ostringstream oss;
        oss << "claude: done in " << std::fixed << std::setprecision(1)
            << (dur_ms / 1000.0) << "s, turns=" << static_cast<int>(turns)
            << ", cost=$" << std::setprecision(4) << cost;
        if (m_saw_error) oss << " ERROR";
        emit(oss.str());
    }
    // rate_limit_event, stream_event, and any unknown types: silently
    // ignored. They're either too noisy or not actionable for progress.
}

} // namespace modmesh_bot
