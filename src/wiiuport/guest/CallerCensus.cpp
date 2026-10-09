#include "wiiuport/guest/CallerCensus.h"

#include "wiiuport/guest/GuestWords.h"
#include "wiiuport/guest/ProbeInstallation.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <system_error>
#include <utility>

namespace wiiuport::guest {

namespace {

std::optional<uint32_t> hex(std::string_view text) {
    uint32_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string hexText(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

} // namespace

std::optional<std::vector<CallerCensus::Target>> CallerCensus::parse(std::string_view text,
                                                                     std::string& refusal) {
    std::vector<Target> targets;
    while (!text.empty()) {
        size_t comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        size_t colon = item.find(':');
        std::optional<uint32_t> entry =
            colon == std::string_view::npos ? std::nullopt : hex(item.substr(0, colon));
        std::optional<uint32_t> first =
            colon == std::string_view::npos ? std::nullopt : hex(item.substr(colon + 1));
        if (!entry.has_value() || !first.has_value()) {
            refusal =
                "each target is entry:firstInstruction in hex, not '" + std::string(item) + "'";
            return std::nullopt;
        }
        targets.push_back({.entry = *entry, .firstInstruction = *first});
    }
    if (targets.size() > kMaxEntries) {
        refusal = "at most 8 targets";
        return std::nullopt;
    }
    return targets;
}

void CallerCensus::install(std::span<const Target> targets) {
    for (const Target& target : targets) {
        m_entries.push_back(std::make_unique<Entry>(target, m_guestBytes));
        m_register(target.entry, target.firstInstruction, *m_entries.back(), true, 0);
    }
}

std::string CallerCensus::json() const {
    std::string body = "{\"entries\":[";
    for (size_t i = 0; i < m_entries.size(); ++i) {
        body += (i == 0 ? "" : ",") + m_entries[i]->json();
    }
    return body + "]}\n";
}

void CallerCensus::Entry::OnInstall(GuestCallProbes::Installation installation) {
    std::scoped_lock lock(m_mutex);
    m_installation = installation;
}

CallerCensus::Chain CallerCensus::chainOf(uint32_t stackPointer, uint32_t returnAddress,
                                          GuestBytes guestBytes) {
    Chain chain{};
    chain[0] = returnAddress;
    // Each frame's word 0 is its caller's frame, whose word 1 holds the link register saved there.
    uint32_t frame = stackPointer;
    for (size_t depth = 1; depth < kChainDepth; ++depth) {
        const void* back = guestBytes(frame, sizeof(uint32_t));
        if (back == nullptr) {
            break;
        }
        frame = guestWord(back, 0);
        const void* saved = frame == 0 ? nullptr : guestBytes(frame, 2 * sizeof(uint32_t));
        if (saved == nullptr) {
            break;
        }
        chain[depth] = guestWord(saved, sizeof(uint32_t));
    }
    return chain;
}

void CallerCensus::Entry::OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) {
    const Chain chain = chainOf(gpr[1], returnAddress, m_guestBytes);
    std::scoped_lock lock(m_mutex);
    ++m_callsByReturn[returnAddress];
    ++m_callsByChain[chain];
}

std::string CallerCensus::Entry::json() const {
    std::vector<std::pair<uint32_t, uint64_t>> callers;
    std::vector<std::pair<Chain, uint64_t>> chains;
    std::string installation;
    {
        std::scoped_lock lock(m_mutex);
        callers.assign(m_callsByReturn.begin(), m_callsByReturn.end());
        chains.assign(m_callsByChain.begin(), m_callsByChain.end());
        installation = installationName(m_installation);
    }
    std::ranges::sort(callers, [](const auto& one, const auto& other) {
        return one.second > other.second;
    });
    uint64_t calls = 0;
    std::string list;
    for (const auto& [returnAddress, count] : callers) {
        calls += count;
        list += (list.empty() ? "" : ",") + std::string("{\"returnAddress\":\"") +
                hexText(returnAddress) + "\",\"calls\":" + std::to_string(count) + "}";
    }
    std::ranges::sort(chains, [](const auto& one, const auto& other) {
        return one.second > other.second;
    });
    std::string chainList;
    for (size_t i = 0; i < chains.size() && i < kReportedChains; ++i) {
        std::string frames;
        for (uint32_t frame : chains[i].first) {
            frames += (frames.empty() ? "\"" : ",\"") + hexText(frame) + "\"";
        }
        chainList += (chainList.empty() ? "" : ",") + std::string("{\"frames\":[") + frames +
                     "],\"calls\":" + std::to_string(chains[i].second) + "}";
    }
    return "{\"entry\":\"" + hexText(m_target.entry) + "\",\"installation\":\"" + installation +
           "\",\"calls\":" + std::to_string(calls) + ",\"callers\":[" + list +
           "],\"distinctChains\":" + std::to_string(chains.size()) + ",\"chains\":[" + chainList +
           "]}";
}

} // namespace wiiuport::guest
