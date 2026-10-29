#pragma once
#include "../../httplib.h"
#include "sse_stream.h"
#include <memory>

namespace HolyHTTP {
inline void InstallSSE(httplib::Response& response, const std::shared_ptr<Stream>& state,
                       std::function<bool()> disconnected) {
    response.body.clear(); response.status = 200;
    // Previously selected buffered headers cannot describe this chunked response.
    for (const auto* name : {"Content-Length", "Transfer-Encoding", "Content-Type", "Content-Encoding", "Cache-Control", "X-Accel-Buffering"})
        response.headers.erase(name);
    response.set_header("Cache-Control", "no-cache, no-transform");
    response.set_header("X-Accel-Buffering", "no");
    // Select identity encoding in Server::write_response_core (see the local httplib patch).
    response.set_header("Content-Encoding", "identity");
    auto lastWrite = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    response.set_chunked_content_provider("text/event-stream",
        [state, disconnected, lastWrite](size_t, httplib::DataSink& sink) {
            while (true) {
                if (disconnected()) { state->Abort("disconnected"); return false; }
                std::string frame;
                const auto next = state->Next(frame);
                if (next == Stream::Poll::Aborted) return false;
                if (next == Stream::Poll::Done) { sink.done(); return true; }
                if (next == Stream::Poll::Idle) {
                    if (std::chrono::steady_clock::now() - *lastWrite < state->heartbeat) continue;
                    frame = ": heartbeat\n\n";
                }
                if (!sink.write(frame.data(), frame.size())) { state->Abort("disconnected"); return false; }
                *lastWrite = std::chrono::steady_clock::now();
                return true;
            }
        }, [state](bool success) { state->Finish(success ? "complete" : "disconnected"); });
}
}
