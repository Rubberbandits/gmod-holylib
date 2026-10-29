#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace HolyConsole {
inline thread_local unsigned deliveryDepth = 0;
struct DeliveryScope { DeliveryScope() { ++deliveryDepth; } ~DeliveryScope() { --deliveryDepth; } };
struct Record {
    uint64_t sequence = 0;
    double timestamp = 0;
    int severity = 0, channelID = -1;
    std::string text, channel, level, cursor;
    unsigned char color[4] = {255, 255, 255, 255};
    bool truncated = false;
};
struct Batch {
    std::vector<Record> records;
    std::string from, cursor;
    bool reset = false, hasMore = false;
    uint64_t dropped = 0;
};
class Buffer {
    mutable std::mutex mutex;
    std::deque<Record> records;
    size_t bytes = 0, byteLimit, recordLimit;
    uint64_t next = 1, dropped = 0, droppedBytes = 0;
    std::string epoch;
    std::string Cursor(uint64_t seq, size_t offset = 0) const {
        return epoch + ":" + std::to_string(seq) + ":" + std::to_string(offset);
    }
public:
    explicit Buffer(size_t maxBytes = 4 * 1024 * 1024, size_t maxRecords = 4096)
        : byteLimit(maxBytes), recordLimit(maxRecords) {
        static std::atomic<uint64_t> instances{0};
        epoch = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++instances);
    }
    void Append(const char* text, size_t length, const std::string& channel = "console",
                const std::string& level = "info", const unsigned char* color = nullptr, int severity = 0, int channelID = -1) {
        if (deliveryDepth || !length) return;
        const double timestamp = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t offset = 0; offset < length; offset += 8192) {
            Record r;
            r.sequence = next++; r.timestamp = timestamp; r.channel = channel; r.level = level;
            r.severity = severity; r.channelID = channelID;
            r.text.assign(text + offset, std::min(size_t(8192), length - offset));
            if (color) std::copy(color, color + 4, r.color);
            bytes += r.text.size(); records.push_back(std::move(r));
            while (!records.empty() && (bytes > byteLimit || records.size() > recordLimit)) {
                bytes -= records.front().text.size(); droppedBytes += records.front().text.size(); records.pop_front(); ++dropped;
            }
        }
    }
    std::string GetCursor() const { std::lock_guard<std::mutex> lock(mutex); return Cursor(next); }
    Batch Read(const std::string& token, size_t maxRecords = 256, size_t maxBytes = 65536) const {
        std::lock_guard<std::mutex> lock(mutex);
        Batch batch;
        const uint64_t earliest = records.empty() ? next : records.front().sequence;
        uint64_t sequence = earliest;
        size_t offset = 0;
        if (!token.empty()) {
            std::string prefix, seqText, offsetText, extra;
            std::istringstream input(token);
            bool valid = bool(std::getline(input, prefix, ':')) && bool(std::getline(input, seqText, ':')) && bool(std::getline(input, offsetText));
            valid = valid && prefix == epoch && !seqText.empty() && !offsetText.empty()
                && seqText.find_first_not_of("0123456789") == std::string::npos && offsetText.find_first_not_of("0123456789") == std::string::npos;
            try { if (valid) { sequence = std::stoull(seqText); offset = std::stoull(offsetText); } } catch (...) { valid = false; }
            if (!valid || sequence < earliest || sequence > next || (sequence == next && offset != 0)
                || (sequence < next && offset > records[size_t(sequence - earliest)].text.size())) {
                batch.reset = true;
                batch.dropped = valid && sequence < earliest ? earliest - sequence : dropped;
                sequence = earliest; offset = 0;
            }
        }
        batch.from = Cursor(sequence, offset);
        size_t used = 0;
        for (const auto& source : records) {
            if (source.sequence < sequence) continue;
            if (batch.records.size() >= maxRecords || used >= maxBytes) break;
            const size_t start = source.sequence == sequence ? offset : 0;
            const size_t take = std::min(source.text.size() - start, maxBytes - used);
            if (take) {
                Record r = source; r.text = source.text.substr(start, take);
                r.cursor = start + take == source.text.size() ? Cursor(source.sequence + 1) : Cursor(source.sequence, start + take);
                batch.records.push_back(std::move(r)); used += take;
            }
            if (start + take == source.text.size()) { sequence = source.sequence + 1; offset = 0; }
            else { sequence = source.sequence; offset = start + take; break; }
        }
        batch.cursor = Cursor(sequence, offset);
        batch.hasMore = sequence < next;
        return batch;
    }
    struct Stats { size_t bytes, records, maxBytes, maxRecords; uint64_t dropped, droppedBytes; std::string earliest, cursor; };
    Stats GetStats() const {
        std::lock_guard<std::mutex> lock(mutex);
        return {bytes, records.size(), byteLimit, recordLimit, dropped, droppedBytes, Cursor(records.empty() ? next : records.front().sequence), Cursor(next)};
    }
};
}
