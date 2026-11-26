#include "assert.hpp"
#include "server_http.hpp"
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace SimpleWeb;

struct Script {
  std::vector<std::string> reads;
  std::size_t index = 0;
  std::size_t offset = 0;
  std::string writes;
};

// Fixed read completions exercise decoder branches without TCP packet assumptions.
class ScriptedSocket {
  io_context &context;
  asio::ip::tcp::socket socket;
  std::shared_ptr<Script> script;

public:
#if (defined(ASIO_STANDALONE) && ASIO_VERSION >= 101300) || BOOST_ASIO_VERSION >= 101300
  using executor_type = asio::ip::tcp::socket::executor_type;
#endif

  ScriptedSocket(io_context &context_, std::shared_ptr<Script> script_)
      : context(context_), socket(context_), script(std::move(script_)) {}

#if (defined(ASIO_STANDALONE) && ASIO_VERSION >= 101300) || BOOST_ASIO_VERSION >= 101300
  executor_type get_executor() noexcept { return socket.get_executor(); }
#else
  io_context &get_io_service() noexcept { return context; }
#endif
  asio::ip::tcp::socket &lowest_layer() noexcept { return socket; }

  template <typename Buffers, typename Handler>
  void async_read_some(const Buffers &buffers, Handler handler) {
    std::size_t count = 0;
    error_code ec;
    if(asio::buffer_size(buffers) == 0) {
      // Composed read_until operations use a zero-byte read for buffered completion.
    }
    else if(script->index == script->reads.size())
      ec = asio::error::eof;
    else {
      auto &part = script->reads[script->index];
      count = asio::buffer_copy(buffers, asio::buffer(part.data() + script->offset, part.size() - script->offset));
      script->offset += count;
      if(script->offset == part.size()) {
        ++script->index;
        script->offset = 0;
      }
    }
    SimpleWeb::post(context, [handler, ec, count]() mutable { handler(ec, count); });
  }

  template <typename Buffers, typename Handler>
  void async_write_some(const Buffers &buffers, Handler handler) {
    const auto count = asio::buffer_size(buffers);
    script->writes.append(asio::buffers_begin(buffers), asio::buffers_end(buffers));
    SimpleWeb::post(context, [handler, count]() mutable { handler(error_code(), count); });
  }
};

class Decoder : public ServerBase<ScriptedSocket> {
  void accept() override {}

public:
  Decoder() : ServerBase<ScriptedSocket>(0) {
    io_service = std::make_shared<io_context>();
    config.timeout_request = 0;
    config.timeout_content = 0;
  }

  void decode(std::shared_ptr<Script> script, const std::string &buffered, std::size_t limit, bool pipeline, bool read_headers) {
    config.max_request_streambuf_size = limit;
    auto connection = create_connection(*io_service, std::move(script));
    auto session = std::make_shared<Session>(limit, connection);
    if(read_headers)
      read(session);
    else {
      session->request->method = "POST";
      session->request->path = "/chunked";
      session->request->http_version = "1.1";
      if(!pipeline)
        session->request->header.emplace("Connection", "close");
      auto buffer = std::make_shared<asio::streambuf>(4096);
      buffer->commit(asio::buffer_copy(buffer->prepare(buffered.size()), asio::buffer(buffered)));
      read_chunked_transfer_encoded(session, buffer);
    }
    io_service->run();
  }
};

struct Result {
  std::vector<std::string> bodies;
  std::vector<std::string> paths;
  std::vector<error_code> errors;
  std::string writes;
};

Result decode(const std::string &buffered, std::vector<std::string> parts = {}, std::size_t limit = 128, bool pipeline = false, bool read_headers = false) {
  Result result;
  auto script = std::make_shared<Script>();
  script->reads = std::move(parts);
  Decoder decoder;
  decoder.default_resource["POST"] = [&result](std::shared_ptr<Decoder::Response> response, std::shared_ptr<Decoder::Request> request) {
    result.bodies.push_back(request->content.string());
    result.paths.push_back(request->path);
    response->write("ok");
  };
  decoder.on_error = [&result](std::shared_ptr<Decoder::Request>, const error_code &ec) { result.errors.push_back(ec); };
  decoder.decode(script, buffered, limit, pipeline, read_headers);
  result.writes = script->writes;
  return result;
}

void rejected(const char *name, const std::string &buffered, std::vector<std::string> parts = {}, std::size_t limit = 128) {
  std::cout << name << std::endl;
  const auto result = decode(buffered, std::move(parts), limit);
  ASSERT(result.bodies.empty());
  ASSERT(!result.errors.empty());
  ASSERT(result.writes.find("HTTP/1.1 200") == std::string::npos);
}

void accepted(const char *name, const std::string &buffered, std::vector<std::string> parts = {}, const std::string &body = "x", std::size_t limit = 128) {
  std::cout << name << std::endl;
  const auto result = decode(buffered, std::move(parts), limit);
  ASSERT(result.bodies == std::vector<std::string>{body});
  ASSERT(result.errors.empty());
  ASSERT(result.writes.find("HTTP/1.1 200") != std::string::npos);
}
} // namespace

int main() {
  rejected("size suffix", "1junk\r\nx\r\n0\r\n\r\n");
  rejected("positive sign", "+1\r\nx\r\n0\r\n\r\n");
  rejected("negative sign", "-1\r\nx\r\n0\r\n\r\n");
  rejected("hex prefix", "0x1\r\nx\r\n0\r\n\r\n");
  rejected("leading whitespace", " 1\r\nx\r\n0\r\n\r\n");
  rejected("trailing whitespace", "1 \r\nx\r\n0\r\n\r\n");
  rejected("embedded LF", "1\nx\r\nx\r\n0\r\n\r\n");
  rejected("empty extension name", "1;\r\nx\r\n0\r\n\r\n");
  rejected("empty extension value", "1;a=\r\nx\r\n0\r\n\r\n");
  rejected("unterminated quoted extension", "1;a=\"x\r\nx\r\n0\r\n\r\n");
  rejected("extension control character", "1;a=\"\x01\"\r\nx\r\n0\r\n\r\n");
  rejected("size overflow", "ffffffffffffffffffffffffffffffff\r\n");
  rejected("buffered delimiter", "1\r\nx!!0\r\n\r\n");
  rejected("body read delimiter", "1\r\n", {"x", "!", "!", "0\r\n\r\n"});
  rejected("one buffered delimiter byte", "1\r\nx!", {"!0\r\n\r\n"});
  rejected("unread delimiter", "1\r\nx", {"!!", "0\r\n\r\n"});
  rejected("invalid second delimiter byte", "1\r\nx\r", {"!0\r\n\r\n"});
  rejected("zero chunk delimiter", "0\r\n!!");
  rejected("nonempty trailer", "0\r\nX-Foo: bar\r\n\r\n");
  rejected("missing terminal delimiter", "0\r\n");
  rejected("truncated body", "2\r\nx");
  rejected("cumulative content bound", "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", {}, 4);
  rejected("remaining-capacity overflow", "1\r\nx\r\nffffffffffffffff\r\n", {}, 4);

  accepted("fully buffered", "1\r\nx\r\n0\r\n\r\n");
  accepted("body then delimiter", "1\r\n", {"x", "\r", "\n", "0\r\n", "\r", "\n"});
  accepted("one buffered CR", "1\r\nx\r", {"\n0\r\n\r\n"});
  accepted("unread CRLF", "1\r\nx", {"\r\n0\r\n\r\n"});
  accepted("split size line", "", {"1", "\r", "\n", "x", "\r", "\n", "0", "\r", "\n", "\r", "\n"});
  accepted("uppercase hex", "A\r\n0123456789\r\n0\r\n\r\n", {}, "0123456789");
  accepted("zero body", "0\r\n\r\n", {}, "");
  accepted("token extension", "1;foo=bar\r\nx\r\n0\r\n\r\n");
  accepted("BWS extension", "1 \t; foo = bar\r\nx\r\n0\r\n\r\n");
  accepted("quoted extension", "1;f=\"a\\\"b\"\r\nx\r\n0\r\n\r\n");
  accepted("exact body bound", "2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n", {}, "abcd", 4);

  std::cout << "long fragmented extension through HTTP entry point" << std::endl;
  const auto extended = decode("", {"POST /chunked HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
                                    "1;extension-name=", "a-long-extension-value", "\r", "\n", "x\r\n0\r\n\r\n"}, 128, false, true);
  ASSERT(extended.bodies == std::vector<std::string>{"x"});
  ASSERT(extended.errors.empty());

  std::cout << "extension line constrained by request limit" << std::endl;
  const auto excessive = decode("", {"POST /chunked HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
                                     "1;name=", std::string(128, 'x'), "\r\nx\r\n0\r\n\r\n"}, 128, false, true);
  ASSERT(excessive.bodies.empty());
  ASSERT(!excessive.errors.empty());

  std::cout << "pipelined request bytes" << std::endl;
  const auto pipeline = decode("1\r\nx\r\n0\r\n\r\nPOST /next HTTP/1.1\r\nContent-Length: 1\r\nConnection: close\r\n\r\ny", {}, 128, true);
  ASSERT(pipeline.bodies == (std::vector<std::string>{"x", "y"}));
  ASSERT(pipeline.paths == (std::vector<std::string>{"/chunked", "/next"}));
  ASSERT(pipeline.errors.empty());
}
