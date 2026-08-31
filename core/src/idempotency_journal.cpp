#include "industrial_bus/core/idempotency_journal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace industrial_bus::core {
namespace {

constexpr std::uint32_t kMagic = 0x524a4249U;  // "IBJR" in little endian.
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kRegistrationRecord = 1;
constexpr std::uint16_t kStateRecord = 2;
constexpr std::size_t kHeaderSize = 16;
constexpr std::uint32_t kMaxRecordSize = 16U * 1024U * 1024U;

std::int64_t unixNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Error ioError(const std::string& action, int error_number = errno) {
    return Error::fileIo(action + ": " + std::strerror(error_number), error_number,
                         error_number == EINTR || error_number == EAGAIN ||
                             error_number == ENOSPC);
}

void appendU16(ByteBuffer& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendU32(ByteBuffer& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendU64(ByteBuffer& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

bool readU16(const ByteBuffer& input, std::size_t& offset, std::uint16_t& value) {
    if (input.size() - offset < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(input[offset]) |
            static_cast<std::uint16_t>(input[offset + 1]) << 8U;
    offset += 2;
    return true;
}

bool readU32(const ByteBuffer& input, std::size_t& offset, std::uint32_t& value) {
    if (input.size() - offset < 4) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(input[offset++]) << shift;
    }
    return true;
}

bool readU64(const ByteBuffer& input, std::size_t& offset, std::uint64_t& value) {
    if (input.size() - offset < 8) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(input[offset++]) << shift;
    }
    return true;
}

bool appendString(ByteBuffer& output, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
    return true;
}

bool readString(const ByteBuffer& input, std::size_t& offset, std::string& value) {
    std::uint32_t size = 0;
    if (!readU32(input, offset, size) || input.size() - offset < size) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(input.data() + offset), size);
    offset += size;
    return true;
}

std::uint32_t crc32(const ByteBuffer& data) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::uint32_t rotateRight(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32U - count));
}

std::string sha256(const ByteBuffer& input) {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U,
                                      0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};

    ByteBuffer padded = input;
    padded.push_back(0x80U);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    const auto bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(bit_length >> shift));
    }

    for (std::size_t block = 0; block < padded.size(); block += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto at = block + i * 4;
            words[i] = static_cast<std::uint32_t>(padded[at]) << 24U |
                       static_cast<std::uint32_t>(padded[at + 1]) << 16U |
                       static_cast<std::uint32_t>(padded[at + 2]) << 8U |
                       static_cast<std::uint32_t>(padded[at + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = rotateRight(words[i - 15], 7) ^
                            rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3U);
            const auto s1 = rotateRight(words[i - 2], 17) ^
                            rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];
        auto f = hash[5];
        auto g = hash[6];
        auto h = hash[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto upper_e = rotateRight(e, 6) ^ rotateRight(e, 11) ^
                                 rotateRight(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + upper_e + choice + constants[i] + words[i];
            const auto upper_a = rotateRight(a, 2) ^ rotateRight(a, 13) ^
                                 rotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = upper_a + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto word : hash) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto byte = static_cast<std::uint8_t>(word >> shift);
            result.push_back(hex[byte >> 4U]);
            result.push_back(hex[byte & 0x0fU]);
        }
    }
    return result;
}

ByteBuffer frameRecord(std::uint16_t type, ByteBuffer payload) {
    ByteBuffer result;
    result.reserve(kHeaderSize + payload.size());
    appendU32(result, kMagic);
    appendU16(result, kVersion);
    appendU16(result, type);
    appendU32(result, static_cast<std::uint32_t>(payload.size()));
    appendU32(result, crc32(payload));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

ByteBuffer serializeRegistration(const IdempotencyRecord& record) {
    ByteBuffer payload;
    appendU64(payload, static_cast<std::uint64_t>(record.created_at_unix_ns));
    appendU64(payload, static_cast<std::uint64_t>(record.expires_at_unix_ns));
    appendU32(payload, static_cast<std::uint32_t>(record.state));
    appendString(payload, record.identity.source_service);
    appendString(payload, record.identity.command_topic);
    appendString(payload, record.identity.idempotency_key);
    appendString(payload, record.payload_sha256);
    appendString(payload, record.operation_id);
    appendString(payload, record.owner_service);
    appendString(payload, record.owner_instance);

    return frameRecord(kRegistrationRecord, std::move(payload));
}

Result<IdempotencyRecord> deserializeRegistration(const ByteBuffer& payload) {
    IdempotencyRecord record;
    std::size_t offset = 0;
    std::uint64_t created = 0;
    std::uint64_t expires = 0;
    std::uint32_t state = 0;
    if (!readU64(payload, offset, created) || !readU64(payload, offset, expires) ||
        !readU32(payload, offset, state) ||
        !readString(payload, offset, record.identity.source_service) ||
        !readString(payload, offset, record.identity.command_topic) ||
        !readString(payload, offset, record.identity.idempotency_key) ||
        !readString(payload, offset, record.payload_sha256) ||
        !readString(payload, offset, record.operation_id) ||
        !readString(payload, offset, record.owner_service) ||
        !readString(payload, offset, record.owner_instance) || offset != payload.size()) {
        return Error::protocol("invalid idempotency journal registration record");
    }
    if (created > static_cast<std::uint64_t>(INT64_MAX) ||
        expires > static_cast<std::uint64_t>(INT64_MAX) ||
        state == static_cast<std::uint32_t>(OperationState::Unspecified) ||
        state > static_cast<std::uint32_t>(OperationState::Cancelled) ||
        record.identity.source_service.empty() ||
        record.identity.command_topic.empty() ||
        record.identity.idempotency_key.empty() || record.payload_sha256.size() != 64 ||
        record.operation_id.empty()) {
        return Error::protocol("idempotency journal record contains invalid fields");
    }
    record.created_at_unix_ns = static_cast<std::int64_t>(created);
    record.updated_at_unix_ns = record.created_at_unix_ns;
    record.expires_at_unix_ns = static_cast<std::int64_t>(expires);
    record.state = static_cast<OperationState>(state);
    return record;
}

struct StateUpdateRecord {
    CommandIdentity identity;
    std::string operation_id;
    OperationState state{OperationState::Unspecified};
    std::int64_t updated_at_unix_ns{0};
    std::optional<Error> error;
};

ByteBuffer serializeStateUpdate(const StateUpdateRecord& update) {
    ByteBuffer payload;
    appendU64(payload, static_cast<std::uint64_t>(update.updated_at_unix_ns));
    appendU32(payload, static_cast<std::uint32_t>(update.state));
    appendString(payload, update.identity.source_service);
    appendString(payload, update.identity.command_topic);
    appendString(payload, update.identity.idempotency_key);
    appendString(payload, update.operation_id);
    appendU32(payload, update.error ? 1U : 0U);
    if (update.error) {
        appendU32(payload, static_cast<std::uint32_t>(update.error->category));
        appendU32(payload, static_cast<std::uint32_t>(update.error->code));
        appendU32(payload, update.error->retryable ? 1U : 0U);
        appendString(payload, update.error->message);
        appendString(payload, update.error->operation_id);
        appendU32(payload, static_cast<std::uint32_t>(update.error->context.size()));
        for (const auto& item : update.error->context) {
            appendString(payload, item.first);
            appendString(payload, item.second);
        }
    }
    return frameRecord(kStateRecord, std::move(payload));
}

Result<StateUpdateRecord> deserializeStateUpdate(const ByteBuffer& payload) {
    StateUpdateRecord update;
    std::size_t offset = 0;
    std::uint64_t updated = 0;
    std::uint32_t state = 0;
    std::uint32_t has_error = 0;
    if (!readU64(payload, offset, updated) || !readU32(payload, offset, state) ||
        !readString(payload, offset, update.identity.source_service) ||
        !readString(payload, offset, update.identity.command_topic) ||
        !readString(payload, offset, update.identity.idempotency_key) ||
        !readString(payload, offset, update.operation_id) ||
        !readU32(payload, offset, has_error) || has_error > 1U) {
        return Error::protocol("invalid idempotency journal state record");
    }
    if (updated > static_cast<std::uint64_t>(INT64_MAX) ||
        state == static_cast<std::uint32_t>(OperationState::Unspecified) ||
        state > static_cast<std::uint32_t>(OperationState::Cancelled) ||
        update.identity.source_service.empty() ||
        update.identity.command_topic.empty() ||
        update.identity.idempotency_key.empty() || update.operation_id.empty()) {
        return Error::protocol("idempotency state record contains invalid fields");
    }
    update.updated_at_unix_ns = static_cast<std::int64_t>(updated);
    update.state = static_cast<OperationState>(state);
    if (has_error != 0U) {
        Error error;
        std::uint32_t category = 0;
        std::uint32_t code = 0;
        std::uint32_t retryable = 0;
        std::uint32_t context_size = 0;
        if (!readU32(payload, offset, category) || !readU32(payload, offset, code) ||
            !readU32(payload, offset, retryable) || retryable > 1U ||
            !readString(payload, offset, error.message) ||
            !readString(payload, offset, error.operation_id) ||
            !readU32(payload, offset, context_size) ||
            category > static_cast<std::uint32_t>(ErrorCategory::ServiceAlreadyRegistered) ||
            context_size > 1024U) {
            return Error::protocol("invalid Error in idempotency state record");
        }
        error.category = static_cast<ErrorCategory>(category);
        error.code = static_cast<std::int32_t>(code);
        error.retryable = retryable != 0U;
        for (std::uint32_t i = 0; i < context_size; ++i) {
            std::string key;
            std::string value;
            if (!readString(payload, offset, key) ||
                !readString(payload, offset, value) || key.empty() ||
                !error.context.emplace(std::move(key), std::move(value)).second) {
                return Error::protocol("invalid Error context in state record");
            }
        }
        update.error = std::move(error);
    }
    if (offset != payload.size() ||
        (update.error && update.state != OperationState::Failed)) {
        return Error::protocol("idempotency state record has trailing or invalid data");
    }
    return update;
}

bool sameIdentity(const CommandIdentity& left, const CommandIdentity& right) {
    return left.source_service == right.source_service &&
           left.command_topic == right.command_topic &&
           left.idempotency_key == right.idempotency_key;
}

struct IdentityHash {
    std::size_t operator()(const CommandIdentity& value) const noexcept {
        std::size_t seed = std::hash<std::string>{}(value.source_service);
        seed ^= std::hash<std::string>{}(value.command_topic) + 0x9e3779b9U +
                (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<std::string>{}(value.idempotency_key) + 0x9e3779b9U +
                (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

struct IdentityEqual {
    bool operator()(const CommandIdentity& left,
                    const CommandIdentity& right) const noexcept {
        return sameIdentity(left, right);
    }
};

Result<void> preadExact(int fd, void* output, std::size_t size, off_t offset) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    while (size != 0) {
        const auto count = ::pread(fd, cursor, size, offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return count == 0 ? Error::protocol("unexpected end of journal")
                              : ioError("failed to read idempotency journal");
        }
        cursor += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
    }
    return {};
}

Result<void> pwriteExact(int fd, const ByteBuffer& data, off_t offset) {
    std::size_t remaining = data.size();
    const auto* cursor = data.data();
    while (remaining != 0) {
        const auto count = ::pwrite(fd, cursor, remaining, offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return ioError("failed to append idempotency journal");
        }
        cursor += count;
        remaining -= static_cast<std::size_t>(count);
        offset += count;
    }
    return {};
}

std::string parentPath(const std::string& path) {
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return ".";
    }
    return separator == 0 ? "/" : path.substr(0, separator);
}

Result<void> syncParentDirectory(const std::string& path) {
    const auto parent = parentPath(path);
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return ioError("failed to open journal parent directory");
    }
    const int result = ::fsync(directory);
    const int saved_errno = errno;
    ::close(directory);
    return result == 0 ? Result<void>{}
                       : Result<void>{ioError("failed to sync journal parent directory",
                                              saved_errno)};
}

}  // namespace

class IdempotencyJournal::Impl {
public:
    explicit Impl(IdempotencyJournalOptions options_value)
        : options(std::move(options_value)) {}

    ~Impl() {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    }

    Result<void> initialize() {
        struct stat before {};
        const bool existed = ::lstat(options.path.c_str(), &before) == 0;
        if (existed && !S_ISREG(before.st_mode)) {
            return Error::fileIo("idempotency journal path is not a regular file");
        }
        fd = ::open(options.path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                    S_IRUSR | S_IWUSR);
        if (fd < 0) {
            return ioError("failed to open idempotency journal");
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            return ioError("idempotency journal is already in use");
        }

        struct stat info {};
        if (::fstat(fd, &info) != 0) {
            return ioError("failed to inspect idempotency journal");
        }
        if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid()) {
            return Error::permissionDenied(
                "idempotency journal must be a regular file owned by this user");
        }
        if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            return Error::permissionDenied(
                "idempotency journal permissions must not grant group or other access");
        }
        if (!existed) {
            auto synced = syncParentDirectory(options.path);
            if (!synced) {
                return synced;
            }
        }
        return recover(info.st_size);
    }

    Result<void> recover(off_t file_size) {
        off_t offset = 0;
        const auto now = unixNowNs();
        while (offset < file_size) {
            if (file_size - offset < static_cast<off_t>(kHeaderSize)) {
                return truncateTail(offset);
            }
            ByteBuffer header(kHeaderSize);
            auto header_result = preadExact(fd, header.data(), header.size(), offset);
            if (!header_result) {
                return header_result;
            }
            std::size_t header_offset = 0;
            std::uint32_t magic = 0;
            std::uint16_t version = 0;
            std::uint16_t type = 0;
            std::uint32_t payload_size = 0;
            std::uint32_t checksum = 0;
            if (!readU32(header, header_offset, magic) ||
                !readU16(header, header_offset, version) ||
                !readU16(header, header_offset, type) ||
                !readU32(header, header_offset, payload_size) ||
                !readU32(header, header_offset, checksum) || magic != kMagic ||
                version != kVersion ||
                (type != kRegistrationRecord && type != kStateRecord) ||
                payload_size > kMaxRecordSize) {
                return Error::protocol("unsupported or corrupt idempotency journal header");
            }
            const auto record_size = static_cast<off_t>(kHeaderSize + payload_size);
            if (file_size - offset < record_size) {
                return truncateTail(offset);
            }
            ByteBuffer payload(payload_size);
            auto payload_result = preadExact(fd, payload.data(), payload.size(),
                                             offset + kHeaderSize);
            if (!payload_result) {
                return payload_result;
            }
            if (crc32(payload) != checksum) {
                return Error::protocol("idempotency journal checksum mismatch");
            }
            if (type == kRegistrationRecord) {
                auto decoded = deserializeRegistration(payload);
                if (!decoded) {
                    return decoded.error();
                }
                auto record = std::move(decoded.value());
                if (record.expires_at_unix_ns > now) {
                    const auto inserted = records.emplace(record.identity, record);
                    if (!inserted.second) {
                        return Error::protocol(
                            "idempotency journal contains overlapping live registrations");
                    }
                }
            } else {
                auto decoded = deserializeStateUpdate(payload);
                if (!decoded) {
                    return decoded.error();
                }
                auto update = std::move(decoded.value());
                const auto existing = records.find(update.identity);
                if (existing != records.end() &&
                    existing->second.operation_id == update.operation_id) {
                    if (update.updated_at_unix_ns < existing->second.updated_at_unix_ns ||
                        (update.state != existing->second.state &&
                         !isValidTransition(existing->second.state, update.state))) {
                        return Error::protocol(
                            "idempotency journal contains an invalid state transition");
                    }
                    existing->second.state = update.state;
                    existing->second.updated_at_unix_ns = update.updated_at_unix_ns;
                    existing->second.error = std::move(update.error);
                }
            }
            offset += record_size;
        }
        file_end = file_size;
        return {};
    }

    Result<void> truncateTail(off_t valid_size) {
        if (::ftruncate(fd, valid_size) != 0 || ::fdatasync(fd) != 0) {
            return ioError("failed to remove partial idempotency journal tail");
        }
        file_end = valid_size;
        return {};
    }

    void purgeExpired(std::int64_t now) {
        for (auto item = records.begin(); item != records.end();) {
            if (item->second.expires_at_unix_ns <= now) {
                item = records.erase(item);
            } else {
                ++item;
            }
        }
    }

    IdempotencyJournalOptions options;
    int fd{-1};
    off_t file_end{0};
    mutable std::mutex mutex;
    std::unordered_map<CommandIdentity, IdempotencyRecord, IdentityHash, IdentityEqual>
        records;
};

Result<std::unique_ptr<IdempotencyJournal>> IdempotencyJournal::open(
    IdempotencyJournalOptions options) {
    constexpr std::int64_t nanoseconds_per_millisecond = 1000000;
    if (options.path.empty() || options.retention <= std::chrono::milliseconds::zero() ||
        options.retention.count() >
            std::numeric_limits<std::int64_t>::max() /
                nanoseconds_per_millisecond ||
        options.max_live_records == 0) {
        return Error::invalidArgument(
            "journal path, positive retention and capacity are required");
    }
    auto impl = std::make_unique<Impl>(std::move(options));
    auto initialized = impl->initialize();
    if (!initialized) {
        return initialized.error();
    }
    return std::unique_ptr<IdempotencyJournal>(
        new IdempotencyJournal(std::move(impl)));
}

IdempotencyJournal::IdempotencyJournal(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

IdempotencyJournal::~IdempotencyJournal() = default;

Result<RegistrationDecision> IdempotencyJournal::registerCommand(
    CommandRegistration command) {
    if (command.identity.source_service.empty() ||
        command.identity.command_topic.empty() ||
        command.identity.idempotency_key.empty() || command.operation_id.empty() ||
        command.owner_service.empty() || command.owner_instance.empty() ||
        command.initial_state != OperationState::Created) {
        return Error::invalidArgument("command registration is missing required fields");
    }
    const auto digest = sha256(command.payload);
    const auto now = unixNowNs();
    const auto retention_ns = impl_->options.retention.count() * 1000000;
    if (now > std::numeric_limits<std::int64_t>::max() - retention_ns) {
        return Error::invalidArgument("command registration timestamp overflows retention");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->purgeExpired(now);
    const auto existing = impl_->records.find(command.identity);
    if (existing != impl_->records.end()) {
        if (existing->second.payload_sha256 != digest) {
            auto error = Error::idempotencyConflict(
                "idempotency key was already registered with another payload");
            error.operation_id = existing->second.operation_id;
            return error;
        }
        return RegistrationDecision{existing->second, true};
    }
    if (impl_->records.size() >= impl_->options.max_live_records) {
        return Error::resourceExhausted("idempotency journal live-record capacity reached");
    }

    IdempotencyRecord record;
    record.identity = std::move(command.identity);
    record.payload_sha256 = digest;
    record.operation_id = std::move(command.operation_id);
    record.owner_service = std::move(command.owner_service);
    record.owner_instance = std::move(command.owner_instance);
    record.state = command.initial_state;
    record.created_at_unix_ns = now;
    record.updated_at_unix_ns = now;
    record.expires_at_unix_ns = now + retention_ns;
    const auto serialized = serializeRegistration(record);
    if (serialized.size() - kHeaderSize > kMaxRecordSize) {
        return Error::resourceExhausted("idempotency journal record is too large");
    }

    const auto original_end = impl_->file_end;
    auto written = pwriteExact(impl_->fd, serialized, original_end);
    if (!written || ::fdatasync(impl_->fd) != 0) {
        const Error error = !written ? written.error()
                                     : ioError("failed to sync idempotency journal");
        (void)::ftruncate(impl_->fd, original_end);
        (void)::fdatasync(impl_->fd);
        return error;
    }
    impl_->file_end += static_cast<off_t>(serialized.size());
    impl_->records.emplace(record.identity, record);
    return RegistrationDecision{std::move(record), false};
}

Result<IdempotencyRecord> IdempotencyJournal::updateState(
    const CommandIdentity& identity,
    const std::string& operation_id,
    OperationState next,
    std::optional<Error> error) {
    if (identity.source_service.empty() || identity.command_topic.empty() ||
        identity.idempotency_key.empty() || operation_id.empty() ||
        next == OperationState::Unspecified ||
        (error && next != OperationState::Failed) ||
        (next == OperationState::Failed && (!error || !*error))) {
        return Error::invalidArgument("invalid idempotency state update");
    }
    const auto now = unixNowNs();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto existing = impl_->records.find(identity);
    if (existing == impl_->records.end() || existing->second.expires_at_unix_ns <= now) {
        return Error::serviceUnavailable("idempotency record was not found");
    }
    if (existing->second.operation_id != operation_id) {
        return Error::idempotencyConflict(
            "operation id does not own this idempotency record");
    }
    if (existing->second.state == next) {
        return existing->second;
    }
    if (!isValidTransition(existing->second.state, next)) {
        return Error::invalidArgument("invalid authoritative operation state transition");
    }

    StateUpdateRecord update;
    update.identity = identity;
    update.operation_id = operation_id;
    update.state = next;
    update.updated_at_unix_ns =
        std::max(now, existing->second.updated_at_unix_ns);
    update.error = std::move(error);
    const auto serialized = serializeStateUpdate(update);
    if (serialized.size() - kHeaderSize > kMaxRecordSize) {
        return Error::resourceExhausted("idempotency state record is too large");
    }
    const auto original_end = impl_->file_end;
    auto written = pwriteExact(impl_->fd, serialized, original_end);
    if (!written || ::fdatasync(impl_->fd) != 0) {
        const Error write_error = !written
                                      ? written.error()
                                      : ioError("failed to sync idempotency state update");
        (void)::ftruncate(impl_->fd, original_end);
        (void)::fdatasync(impl_->fd);
        return write_error;
    }
    impl_->file_end += static_cast<off_t>(serialized.size());
    existing->second.state = next;
    existing->second.updated_at_unix_ns = now;
    existing->second.error = std::move(update.error);
    return existing->second;
}

Result<IdempotencyRecord> IdempotencyJournal::find(
    const CommandIdentity& identity) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->records.find(identity);
    if (found == impl_->records.end() ||
        found->second.expires_at_unix_ns <= unixNowNs()) {
        return Error::serviceUnavailable("idempotency record was not found");
    }
    return found->second;
}

std::size_t IdempotencyJournal::liveRecordCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto now = unixNowNs();
    std::size_t count = 0;
    for (const auto& item : impl_->records) {
        if (item.second.expires_at_unix_ns > now) {
            ++count;
        }
    }
    return count;
}

}  // namespace industrial_bus::core
