#include "industrial_bus/ids.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>

namespace industrial_bus {
namespace {

bool isHex(char c) noexcept {
    const auto value = static_cast<unsigned char>(c);
    return std::isxdigit(value) != 0;
}

std::mt19937_64 makeGenerator() {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(), device(), device(),
                       device(), device()};
    return std::mt19937_64(seed);
}

}  // namespace

std::string generateUuidV4() {
    thread_local auto generator = makeGenerator();
    std::uniform_int_distribution<std::uint32_t> distribution(0, 255);
    std::array<std::uint8_t, 16> bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(distribution(generator));
    }

    // RFC 4122 variant and version bits.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

bool isValidUuid(std::string_view value) noexcept {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? value[index] != '-' : !isHex(value[index])) {
            return false;
        }
    }

    return true;
}

bool isValidProtocolId(std::string_view value, std::size_t max_length) noexcept {
    if (value.empty() || value.size() > max_length) {
        return false;
    }

    for (const char character : value) {
        const bool ascii_alphanumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9');
        if (!(ascii_alphanumeric || character == '.' || character == '_' ||
              character == ':' || character == '-')) {
            return false;
        }
    }
    return true;
}

}  // namespace industrial_bus
