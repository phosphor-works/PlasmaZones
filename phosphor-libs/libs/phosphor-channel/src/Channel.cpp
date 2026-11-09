// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Channel.h>

namespace PhosphorChannel {

bool validCodec(std::string_view codec)
{
    if (codec.empty() || codec.size() > MaxCodecBytes)
        return false;
    for (const char c : codec) {
        if (c <= ' ' || c > '~')
            return false;
    }
    return true;
}

std::string versionMismatch(std::uint32_t theirs)
{
    return "the other end speaks phosphor-channel protocol " + std::to_string(theirs) + ", and this end speaks "
        + std::to_string(ProtocolVersion);
}

} // namespace PhosphorChannel
