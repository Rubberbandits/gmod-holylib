#include "modules/mcp/console_buffer.h"
#include "modules/mcp/http_transport.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (false)
using namespace std::chrono_literals;

static void CaptureTests() {
    HolyConsole::Buffer buffer;
    const auto before = buffer.GetCursor();
    buffer.Append("abcdefghij", 10, "test", "warning");
    auto first = buffer.Read(before, 1, 3);
    CHECK(first.records.size() == 1 && first.records[0].text == "abc" && first.hasMore);
    CHECK(first.records[0].level == "warning" && first.records[0].channel == "test");
    auto rest = buffer.Read(first.cursor, 8, 100);
    CHECK(rest.records[0].text == "defghij" && !rest.hasMore);
    { HolyConsole::DeliveryScope guard; buffer.Append("ignored observer feedback", 25); }
    CHECK(buffer.GetStats().records == 1);
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) writers.emplace_back([&] { for (int j = 0; j < 200; ++j) buffer.Append("message", 7); });
    for (auto& thread : writers) thread.join();
    auto batch = buffer.Read(before, 4096, 1024 * 1024);
    CHECK(batch.records.size() == 1601);
    for (size_t i = 1; i < batch.records.size(); ++i) CHECK(batch.records[i].sequence == batch.records[i-1].sequence + 1);
    HolyConsole::Buffer small(20, 2);
    const auto old = small.GetCursor();
    for (int i = 0; i < 5; ++i) small.Append("1234567890", 10);
    auto reset = small.Read(old);
    CHECK(reset.reset && reset.dropped == 3 && reset.records.size() == 2);
    CHECK(small.GetStats().droppedBytes == 30);
    CHECK(small.Read("file-era:10").reset);
    HolyConsole::Buffer restarted;
    CHECK(restarted.Read(buffer.GetCursor()).reset);
    std::string huge(100000, 'x'); buffer.Append(huge.data(), huge.size());
    auto stats = buffer.GetStats(); CHECK(stats.bytes <= stats.maxBytes && stats.records <= stats.maxRecords);
    auto tail = buffer.Read(rest.cursor, 4096, 1024 * 1024);
    std::string recovered;
    for (const auto& record : tail.records) if (record.text[0] == 'x') { CHECK(record.text.size() <= 8192); recovered += record.text; }
    CHECK(recovered == huge);

    // Readers race eviction and append; each fragment must advance or explicitly
    // reset, and the retained memory bounds must hold throughout.
    HolyConsole::Buffer racing(4096, 64);
    std::atomic<bool> writing{true};
    auto reader = std::async(std::launch::async, [&] {
        auto cursor = racing.GetCursor();
        while (writing.load()) {
            auto part = racing.Read(cursor, 3, 7);
            CHECK(part.records.size() <= 3);
            for (const auto& record : part.records) CHECK(record.text.size() <= 7);
            cursor = part.cursor;
        }
    });
    for (int i = 0; i < 10000; ++i) racing.Append("01234567890123456789", 20);
    writing.store(false); reader.get();
    CHECK(racing.GetStats().bytes <= 4096 && racing.GetStats().records <= 64);

    // Suppression is local to the delivering thread, not a global logging mute.
    auto position = racing.GetCursor();
    { HolyConsole::DeliveryScope scope;
      std::thread engine([&] { racing.Append("other-thread", 12); }); engine.join(); }
    CHECK(racing.Read(position).records[0].text == "other-thread");
}
static void FramingTests() {
    HolyHTTP::Stream stream;
    std::string error, frame;
    CHECK(stream.Event("a\r\nb\rc\n", "message", "cursor", error));
    CHECK(stream.Next(frame) == HolyHTTP::Stream::Poll::Data);
    CHECK(frame == "event: message\nid: cursor\ndata: a\ndata: b\ndata: c\ndata: \n\n");
    CHECK(!stream.Event("x", "bad\nevent", "", error) && error == "invalid_event");
    CHECK(!stream.Event(std::string(1, char(255)), "", "", error));
    CHECK(!stream.Event("x", "", std::string("a\0b", 3), error));
    CHECK(stream.Comment("first\nsecond", error));
    CHECK(stream.Next(frame) == HolyHTTP::Stream::Poll::Data && frame == ": first\n: second\n\n");
    stream.Close(); CHECK(!stream.IsOpen());
    CHECK(stream.Next(frame) == HolyHTTP::Stream::Poll::Done && stream.Reason() == "complete");
    stream.Abort("duplicate"); CHECK(stream.Reason() == "complete");
    HolyHTTP::Stream small(128, 128);
    CHECK(small.Event(std::string(70, 'x'), "", "", error));
    CHECK(!small.Event(std::string(70, 'x'), "", "", error) && error == "backpressure");
    CHECK(small.IsTerminal() && small.Buffered() == 0);
    CHECK(!small.IsFinished());
    small.Finish("disconnected"); CHECK(small.IsFinished() && small.Reason() == "backpressure");
    HolyHTTP::Stream idle;
    auto waiter = std::async(std::launch::async, [&] { std::string data; return idle.Next(data, 10s); });
    idle.Abort("shutdown"); CHECK(waiter.wait_for(1s) == std::future_status::ready);
    CHECK(waiter.get() == HolyHTTP::Stream::Poll::Aborted);
    for (int i = 0; i < 100; ++i) {
        HolyHTTP::Stream raced;
        std::thread writer([&] { std::string e; for (int j = 0; j < 100; ++j) raced.Event("payload", "", "", e); raced.Close(); });
        std::thread cancel([&] { raced.Abort("disconnected"); });
        writer.join(); cancel.join();
        CHECK(raced.IsTerminal() && raced.Buffered() == 0);
        CHECK(!raced.Event("late", "", "", error));
    }
}
static void NetworkTests() {
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(8, 64); };
    server.set_write_timeout(1, 0);
    auto stream = std::make_shared<HolyHTTP::Stream>();
    std::promise<void> firstReceived;
    auto received = firstReceived.get_future();
    server.Get("/stream", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content("old body", "application/json");
        res.set_header("Content-Length", "12345");
        res.set_header("Content-Encoding", "gzip");
        HolyHTTP::InstallSSE(res, stream, req.is_connection_closed);
    });
    std::thread producer([&] {
            std::string error;
            stream->Event("first", "console", "1", error);
            if (received.wait_for(3s) != std::future_status::ready) { stream->Abort("test_timeout"); return; }
            stream->Event("second", "console", "2", error); stream->Close();
    });
    const int port = server.bind_to_any_port("127.0.0.1"); CHECK(port > 0);
    std::thread listener([&] { server.listen_after_bind(); });
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(5, 0);
    std::string text; bool signaled = false;
    auto response = client.Get("/stream", {{"Accept-Encoding", "gzip, br"}}, [&](const char* data, size_t length) {
        text.append(data, length);
        if (!signaled && text.find("first") != std::string::npos) { signaled = true; firstReceived.set_value(); }
        return true;
    });
    if (producer.joinable()) producer.join();
    server.stop(); listener.join();
    CHECK(response && response->status == 200);
    CHECK(response->get_header_value("Content-Type") == "text/event-stream");
    CHECK(response->get_header_value("Content-Encoding") == "identity");
    CHECK(response->get_header_value("X-Accel-Buffering") == "no");
    CHECK(text.find("first") < text.find("second") && signaled);

    // A quiet persistent response must notice disconnect, without waiting for a heartbeat.
    httplib::Server disconnectServer;
    auto disconnected = std::make_shared<HolyHTTP::Stream>();
    disconnectServer.Get("/stream", [&](const httplib::Request& req, httplib::Response& res) {
        HolyHTTP::InstallSSE(res, disconnected, req.is_connection_closed);
        std::string error; disconnected->Event("start", "", "", error);
    });
    const int otherPort = disconnectServer.bind_to_any_port("127.0.0.1"); CHECK(otherPort > 0);
    std::thread otherListener([&] { disconnectServer.listen_after_bind(); });
    httplib::Client dropping("127.0.0.1", otherPort);
    dropping.Get("/stream", [](const char*, size_t) { return false; });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!disconnected->IsTerminal() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    disconnected->Abort("shutdown"); disconnectServer.stop(); otherListener.join();
    CHECK(disconnected->Reason() == "disconnected");

    // Stop is valid between bind and listen thread startup, including immediate
    // listener restart on the same object. This used to leave the socket open.
    for (int i = 0; i < 10; ++i) {
        httplib::Server early;
        early.new_task_queue = [] { return new httplib::ThreadPool(2, 8); };
        CHECK(early.bind_to_any_port("127.0.0.1") > 0);
        early.stop();
        std::thread starting([&] { early.listen_after_bind(); }); starting.join();
        CHECK(early.bind_to_any_port("127.0.0.1") > 0); early.stop();
    }

    httplib::Server heartbeatServer;
    auto heartbeat = std::make_shared<HolyHTTP::Stream>(262144, 65536, 50ms);
    heartbeatServer.Get("/stream", [&](const httplib::Request& req, httplib::Response& res) { HolyHTTP::InstallSSE(res, heartbeat, req.is_connection_closed); });
    const auto heartbeatPort = heartbeatServer.bind_to_any_port("127.0.0.1"); CHECK(heartbeatPort > 0);
    std::thread heartbeatListener([&] { heartbeatServer.listen_after_bind(); });
    httplib::Client quiet("127.0.0.1", heartbeatPort); quiet.set_read_timeout(2, 0);
    bool beat = false;
    quiet.Get("/stream", [&](const char* data, size_t size) { beat = std::string(data, size).find(": heartbeat") != std::string::npos; return false; });
    heartbeat->Abort("shutdown"); heartbeatServer.stop(); heartbeatListener.join(); CHECK(beat);
}
int main() {
    try { CaptureTests(); FramingTests(); NetworkTests(); std::cout << "PASS: native capture, framing, concurrency, incremental HTTP delivery and disconnect\n"; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
