#pragma once

namespace coro_http {

class server;

// Registers the demo endpoints. Each one exists to show exactly one thing:
// plain dispatch, timers, body handling, the blocking bridge, and counters.
void register_demo_routes(server& app);

}  // namespace coro_http
