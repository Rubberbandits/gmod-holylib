#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

namespace HolyHTTP {
inline bool UTF8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char a = text[i];
        size_t n = a < 128 ? 1 : (a >= 194 && a <= 223 ? 2 : (a >= 224 && a <= 239 ? 3 : (a >= 240 && a <= 244 ? 4 : 0)));
        if (!n || i + n > text.size()) return false;
        for (size_t j = 1; j < n; ++j) if ((static_cast<unsigned char>(text[i+j]) & 192) != 128) return false;
        if (n >= 3) { auto b = static_cast<unsigned char>(text[i+1]);
            if ((a == 224 && b < 160) || (a == 237 && b >= 160) || (a == 240 && b < 144) || (a == 244 && b >= 144)) return false; }
        i += n;
    }
    return true;
}
inline bool Field(const std::string& text) { return text.find_first_of("\r\n") == std::string::npos && text.find('\0') == std::string::npos && UTF8(text); }
inline std::string Lines(const std::string& prefix, const std::string& text) {
    std::string result;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
            result += prefix; result.append(text, start, i - start); result += '\n';
            if (i < text.size() && text[i] == '\r' && i + 1 < text.size() && text[i+1] == '\n') ++i;
            start = i + 1;
        }
    }
    return result;
}
class Stream {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    size_t buffered = 0;
    bool closing = false, terminal = false, finished = false;
    std::string reason;
public:
    const size_t maxBuffered, maxEvent;
    const std::chrono::milliseconds heartbeat;
    explicit Stream(size_t bytes = 256 * 1024, size_t event = 64 * 1024, std::chrono::milliseconds interval = std::chrono::seconds(15))
        : maxBuffered(bytes), maxEvent(event), heartbeat(interval) {}
    bool Queue(std::string frame, std::string& failure) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closing || terminal) { failure = "closed"; return false; }
        if (frame.size() > maxEvent) { failure = "too_large"; return false; }
        if (buffered + frame.size() > maxBuffered) {
            terminal = true; reason = "backpressure"; queue.clear(); buffered = 0;
            failure = reason; wake.notify_all(); return false;
        }
        buffered += frame.size(); queue.push_back(std::move(frame)); wake.notify_all(); return true;
    }
    bool Event(const std::string& data, const std::string& event, const std::string& id, std::string& failure) {
        if (!UTF8(data) || !Field(event) || !Field(id)) { failure = "invalid_event"; return false; }
        if (data.size() > maxEvent) { failure = "too_large"; return false; }
        std::string frame;
        if (!event.empty()) frame += "event: " + event + "\n";
        if (!id.empty()) frame += "id: " + id + "\n";
        frame += Lines("data: ", data) + "\n";
        return Queue(std::move(frame), failure);
    }
    bool Comment(const std::string& text, std::string& failure) {
        if (!UTF8(text) || text.size() > maxEvent || text.find('\0') != std::string::npos) { failure = "invalid_comment"; return false; }
        return Queue(Lines(": ", text) + "\n", failure);
    }
    size_t Buffered() const { std::lock_guard<std::mutex> lock(mutex); return buffered; }
    bool IsOpen() const { std::lock_guard<std::mutex> lock(mutex); return !closing && !terminal; }
    bool IsTerminal() const { std::lock_guard<std::mutex> lock(mutex); return terminal; }
    bool IsFinished() const { std::lock_guard<std::mutex> lock(mutex); return finished; }
    // Only the worker/resource releaser (or a joined/never-started request) marks
    // network completion. An abort alone can still have an in-flight write.
    void Finish(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!terminal) { terminal = true; closing = true; reason = why; queue.clear(); buffered = 0; }
        finished = true; wake.notify_all();
    }
    bool WaitUntil(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex);
        return wake.wait_until(lock, deadline, [&] { return finished; });
    }
    std::string Reason() const { std::lock_guard<std::mutex> lock(mutex); return reason; }
    void Close() { std::lock_guard<std::mutex> lock(mutex); closing = true; wake.notify_all(); }
    void Abort(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex);
        if (terminal) return;
        terminal = true; closing = true; reason = why; queue.clear(); buffered = 0; wake.notify_all();
    }
    enum class Poll { Data, Idle, Done, Aborted };
    Poll Next(std::string& frame, std::chrono::milliseconds interval = std::chrono::milliseconds(250)) {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, interval, [&] { return terminal || closing || !queue.empty(); });
        if (terminal) return Poll::Aborted;
        if (!queue.empty()) { frame = std::move(queue.front()); queue.pop_front(); buffered -= frame.size(); return Poll::Data; }
        if (closing) { terminal = true; reason = "complete"; wake.notify_all(); return Poll::Done; }
        return Poll::Idle;
    }
};
}
