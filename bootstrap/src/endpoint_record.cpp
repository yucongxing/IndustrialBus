#include "industrial_bus/bootstrap/endpoint_record.h"

#include "internal.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/random.h>
#include <unistd.h>

namespace industrial_bus::bootstrap {
namespace {

class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { internal::closeNoThrow(fd_); }
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

std::string escapeJson(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (const unsigned char character : input) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20) throw std::invalid_argument("control character in endpoint record");
                output.push_back(static_cast<char>(character));
        }
    }
    return output;
}

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    EndpointRecord parse() {
        EndpointRecord record;
        expect('{');
        bool endpoint = false, pid = false, instance = false, protocol = false, start = false;
        for (;;) {
            skipSpace();
            if (consume('}')) break;
            const std::string key = string();
            expect(':');
            if (key == "endpoint" && !endpoint) { record.endpoint = string(); endpoint = true; }
            else if (key == "pid" && !pid) { record.pid = integer(); pid = true; }
            else if (key == "hub_instance_id" && !instance) { record.hub_instance_id = string(); instance = true; }
            else if (key == "protocol_version" && !protocol) {
                const auto value = integer();
                if (value <= 0 || value > std::numeric_limits<std::uint32_t>::max())
                    throw std::runtime_error("invalid protocol_version in endpoint.json");
                record.protocol_version = static_cast<std::uint32_t>(value); protocol = true;
            } else if (key == "start_time" && !start) { record.start_time = integer(); start = true; }
            else throw std::runtime_error("unknown or duplicate field in endpoint.json: " + key);
            skipSpace();
            if (consume('}')) break;
            expect(',');
        }
        skipSpace();
        if (position_ != input_.size()) throw std::runtime_error("trailing content in endpoint.json");
        if (!(endpoint && pid && instance && protocol && start)) throw std::runtime_error("missing field in endpoint.json");
        if (record.endpoint.empty() || record.pid <= 0 || record.hub_instance_id.empty() || record.start_time <= 0)
            throw std::runtime_error("invalid empty or non-positive field in endpoint.json");
        return record;
    }

private:
    void skipSpace() { while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_; }
    bool consume(char wanted) { skipSpace(); if (position_ < input_.size() && input_[position_] == wanted) { ++position_; return true; } return false; }
    void expect(char wanted) { if (!consume(wanted)) throw std::runtime_error("malformed endpoint.json"); }
    std::string string() {
        skipSpace(); expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const char value = input_[position_++];
            if (value == '"') return output;
            if (static_cast<unsigned char>(value) < 0x20) throw std::runtime_error("control character in endpoint.json");
            if (value != '\\') { output.push_back(value); continue; }
            if (position_ == input_.size()) throw std::runtime_error("bad escape in endpoint.json");
            switch (input_[position_++]) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default: throw std::runtime_error("unsupported escape in endpoint.json");
            }
        }
        throw std::runtime_error("unterminated string in endpoint.json");
    }
    std::int64_t integer() {
        skipSpace();
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ == input_.size() || !std::isdigit(static_cast<unsigned char>(input_[position_])))
            throw std::runtime_error("invalid integer in endpoint.json");
        while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        try { return std::stoll(std::string(input_.substr(begin, position_ - begin))); }
        catch (const std::exception&) { throw std::runtime_error("integer out of range in endpoint.json"); }
    }

    std::string_view input_;
    std::size_t position_{0};
};

bool isValidToken(const std::string& token) {
    if (token.size() != 64) return false;
    for (const char character : token) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) return false;
    }
    return true;
}

}  // namespace

void writeEndpointRecord(const RuntimePaths& paths, const EndpointRecord& record) {
    if (record.endpoint != paths.endpoint()) throw std::invalid_argument("endpoint record does not match RuntimePaths");
    if (record.pid <= 0 || record.hub_instance_id.empty() || record.protocol_version == 0 || record.start_time <= 0)
        throw std::invalid_argument("endpoint record contains an invalid field");
    const std::string json = "{\n  \"endpoint\": \"" + escapeJson(record.endpoint) +
        "\",\n  \"pid\": " + std::to_string(record.pid) +
        ",\n  \"hub_instance_id\": \"" + escapeJson(record.hub_instance_id) +
        "\",\n  \"protocol_version\": " + std::to_string(record.protocol_version) +
        ",\n  \"start_time\": " + std::to_string(record.start_time) + "\n}\n";
    ScopedFd directory(paths.openRuntimeDirectory());
    internal::atomicWriteFile(directory.get(), "endpoint.json", json);
}

EndpointRecord readEndpointRecord(const RuntimePaths& paths) {
    ScopedFd directory(paths.openRuntimeDirectory());
    EndpointRecord record(JsonReader(internal::readSecureFile(directory.get(), "endpoint.json", 16 * 1024)).parse());
    if (record.endpoint != paths.endpoint()) throw std::runtime_error("endpoint.json endpoint does not match runtime directory");
    return record;
}

std::string generateAuthToken() {
    std::array<unsigned char, 32> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            internal::throwErrno("generate authentication token");
        }
        offset += static_cast<std::size_t>(count);
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        token.push_back(hex[byte >> 4]);
        token.push_back(hex[byte & 0x0f]);
    }
    return token;
}

void writeAuthToken(const RuntimePaths& paths, const std::string& token) {
    if (!isValidToken(token)) throw std::invalid_argument("authentication token must be 64 lowercase hexadecimal characters");
    ScopedFd directory(paths.openRuntimeDirectory());
    internal::atomicWriteFile(directory.get(), "auth.token", token);
}

std::string readAuthToken(const RuntimePaths& paths) {
    ScopedFd directory(paths.openRuntimeDirectory());
    const std::string token = internal::readSecureFile(directory.get(), "auth.token", 64);
    if (!isValidToken(token)) throw std::runtime_error("auth.token has invalid format");
    return token;
}

}  // namespace industrial_bus::bootstrap
