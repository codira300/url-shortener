#include "http/HttpServer.h"

#include "observability/Metrics.h"
#include "util/Logger.h"
#include "util/ThreadPool.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace urlshort {

    namespace {
        namespace asio  = boost::asio;
        namespace beast = boost::beast;
        namespace http  = beast::http;
        using tcp = asio::ip::tcp;
        using SV = std::string_view;
        using boost::system::error_code;
        using Clock = std::chrono::steady_clock;

        constexpr SV kServerName = "urlshortener/1.0";
        constexpr std::size_t kMaxTrackedSessions = 1024;
        constexpr auto kShutdownPollDelay = std::chrono::milliseconds(50);

        bool hasHeaderInjection(SV value) {
            return value.find_first_of("\r\n") != SV::npos;
        }

        std::string jsonError(SV code, SV message) {
            std::string body;
            body.reserve(code.size() + message.size() + 40);
            body.append(R"({"error":{"code":")").append(code)
                .append(R"(","message":")").append(message)
                .append(R"("}})");
            return body;
        }


    HttpResponse makeErrorResponse(int status, SV code, SV message, bool keep_alive) {
        HttpResponse response;
        response.status = status;
        response.content_type = "application/json";
        response.body = jsonError(code, message);
        response.keep_alive = keep_alive;
        if (status == 503) {
            response.addHeader("Retry-After", "1");
        }
        return response;
      }
    }  

    class HttpServer::Session : public std::enable_shared_from_this<HttpServer::Session> {

      public:
          friend class HttpServer;

          Session(tcp::socket socket, HttpServer& server)
              : stream_(std::move(socket)),
                strand_(asio::make_strand(stream_.get_executor())),
                server_(server) 
              {
                  error_code ec;
                  stream_.socket().set_option(tcp::no_delay(true), ec);
                  const auto endpoint = stream_.socket().remote_endpoint(ec);
                  peer_ip_ = ec ? std::string("unknown") : endpoint.address().to_string();
              }

          void start() {
              server_.connectionOpened();
              doRead();
          }

          void close() {
              if (closed_.exchange(true, std::memory_order_acq_rel)) return;  

              error_code ec;
              stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
              stream_.socket().close(ec);
              server_.connectionClosed();
          }

      private:
          bool isClosed() const noexcept {
              return closed_.load(std::memory_order_acquire);
          }

          void doRead() {
              if (isClosed()) return;

              parser_.emplace();
              parser_->body_limit(static_cast<std::uint64_t>(server_.config_.max_body_bytes));
              parser_->header_limit(static_cast<std::uint32_t>(server_.config_.max_header_bytes));
              stream_.expires_after(std::chrono::milliseconds(server_.config_.read_timeout_ms));

              http::async_read(
                              stream_, buffer_, *parser_, 
                              asio::bind_executor(strand_, [self = shared_from_this()](error_code ec, std::size_t) {self->onRead(ec);})
                            );
            }

          void onRead(error_code ec) {
              if (isClosed()) return;

              if (ec) {
                  parser_.reset();
                  if (ec == http::error::end_of_stream || ec == asio::error::operation_aborted || ec == asio::error::eof) {
                      close();
                      return;
                  }

                  handleReadError(ec);
                  return;
              }

              auto message = parser_->release();
              parser_.reset();

              dispatchToWorker(buildRequest(std::move(message)));
          }

          void handleReadError(const error_code& ec) {
              server_.metrics_.incHttpParseError();

              int status = 400;
              SV code = "bad_request";
              SV message = "malformed HTTP request";

              if (ec == http::error::body_limit) {
                  status = 413;
                  code = "payload_too_large";
                  message = "request body exceeds the configured limit";
              } else if (ec == http::error::header_limit) {
                  status = 431;
                  code = "header_too_large";
                  message = "request headers exceed the configured limit";
              }
              sendErrorAndClose(status, code, message);
          }

          HttpRequest buildRequest(http::request<http::string_body> message) {
              HttpRequest request;
              request.method = std::string(message.method_string());
              request.target = std::string(message.target());
              request.peer_ip = peer_ip_;
              request.body = std::move(message.body());
              request.keep_alive = message.keep_alive() && !server_.stopping_.load(std::memory_order_acquire);
              request.received_at = Clock::now();

              if (const auto forwarded = message.find("x-forwarded-for"); forwarded != message.end()) {
                  request.forwarded_for = std::string(forwarded->value());
              }
              return request;
          }

          void dispatchToWorker(HttpRequest request) {
              auto response = std::make_shared<HttpResponse>();
              const bool submitted = server_.workers_.submit(
                  [self = shared_from_this(), request = std::move(request), response]() mutable {
                      self->runHandler(request, *response);
                      response->keep_alive =
                          request.keep_alive &&
                          !self->server_.stopping_.load(std::memory_order_acquire);
                      asio::post(self->strand_,
                                [self, response]() mutable { self->finish(std::move(*response)); });
                  });

              if (!submitted) {
                  server_.metrics_.incRequestQueueRejection();
                  sendErrorAndClose(503, "queue_full", "request queue is saturated; retry shortly");
                  return;
              }
              server_.metrics_.setWorkerQueueDepth(
                  static_cast<std::int64_t>(server_.workers_.queueDepth()));
          }

          void runHandler(const HttpRequest& request, HttpResponse& response) {
              try {
                  response = server_.handler_(request);
              } catch (const std::exception& error) {
                  server_.logger_.error("handler_exception", {{"detail", error.what()}});
                  setInternalError(response);
              } catch (...) {
                  server_.logger_.error("handler_exception", {{"detail", "unknown exception"}});
                  setInternalError(response);
              }
          }

          static void setInternalError(HttpResponse& response) {
              response.status = 500;
              response.content_type = "application/json";
              response.body = jsonError("internal_error", "internal server error");
          }

          void sendErrorAndClose(int status, SV code, SV message) {
              finish(makeErrorResponse(status, code, message, /*keep_alive=*/false));
          }

          void finish(HttpResponse response) {
              if (isClosed()) return;

              auto message = std::make_shared<http::response<http::string_body>>(static_cast<http::status>(response.status), 11);
              message->set(http::field::server, kServerName);
              message->set(http::field::content_type, response.content_type);
              message->set("X-Content-Type-Options", "nosniff");
              for (const auto& [name, value] : response.headers) {
                  if (!hasHeaderInjection(name) && !hasHeaderInjection(value)) {
                      message->set(name, value);
                  }
              }
              message->body() = std::move(response.body);
              message->keep_alive(response.keep_alive);
              message->prepare_payload();

              stream_.expires_after(std::chrono::milliseconds(server_.config_.write_timeout_ms));
              const bool keep_alive = response.keep_alive;
              http::async_write(
                  stream_, *message,
                  asio::bind_executor(
                      strand_,
                      [self = shared_from_this(), message, keep_alive](error_code ec, std::size_t) {
                          if (ec || !keep_alive) {
                              self->close();
                              return;
                          }
                          self->doRead();
                      }));
          }

          beast::tcp_stream stream_;
          asio::any_io_executor strand_;
          beast::flat_buffer buffer_;
          std::optional<http::request_parser<http::string_body>> parser_;
          HttpServer& server_;
          std::string peer_ip_;
          std::atomic<bool> closed_{false};
      };

HttpServer::HttpServer(const HttpConfig& config, int shutdown_grace_ms, Handler handler,
                       ThreadPool& workers, Metrics& metrics, Logger& logger)
    : config_(config),
      shutdown_grace_(std::chrono::milliseconds(shutdown_grace_ms)),
      handler_(std::move(handler)),
      workers_(workers),
      metrics_(metrics),
      logger_(logger),
      io_(static_cast<int>(std::max(1, config.io_threads))),
      acceptor_(io_),
      signals_(io_),
      shutdown_timer_(io_) {
    error_code ec;
    const auto address = asio::ip::make_address(config_.host, ec);
    if (ec) {
        throw std::runtime_error("invalid HTTP_HOST: " + config_.host);
    }

    const tcp::endpoint endpoint(address, config_.port);
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        throw std::runtime_error("failed to open listener: " + ec.message());
    }
    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw std::runtime_error("failed to set reuse_address: " + ec.message());
    }
    acceptor_.bind(endpoint, ec);
    if (ec) {
        throw std::runtime_error("failed to bind " + config_.host + ":" +
                                 std::to_string(config_.port) + ": " + ec.message());
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("failed to listen: " + ec.message());
    }
}

HttpServer::~HttpServer() {
    stopping_.store(true, std::memory_order_release);
    error_code ec;
    acceptor_.close(ec);
    io_.stop();
}

void HttpServer::run() {
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
    signals_.async_wait([this](error_code ec, int signal_number) {
        if (!ec) {
            logger_.info("shutdown_signal", {{"signal", std::to_string(signal_number)}});
            beginShutdown();
        }
    });

    doAccept();

    std::vector<std::thread> io_threads;
    const int extra_threads = std::max(0, config_.io_threads - 1);
    io_threads.reserve(static_cast<std::size_t>(extra_threads));
    for (int i = 0; i < extra_threads; ++i) {
        io_threads.emplace_back([this] { io_.run(); });
    }
    io_.run();
    for (auto& thread : io_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    logger_.info("http_server_stopped");
}

void HttpServer::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        io_.stop();
        return;
    }
    asio::post(io_, [this] { beginShutdown(); });
}

void HttpServer::doAccept() {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }

    acceptor_.async_accept(io_, [this](error_code ec, tcp::socket socket) {
        if (!ec) {
            if (active_connections_.load(std::memory_order_acquire) >=
                config_.max_connections) {
                error_code ignored;
                socket.close(ignored);
            } else {
                auto session = std::make_shared<Session>(std::move(socket), *this);
                rememberSession(session);
                session->start();
            }
        } else if (!stopping_.load(std::memory_order_acquire)) {
            logger_.warn("accept_failed", {{"detail", ec.message()}});
        }

        if (!stopping_.load(std::memory_order_acquire)) {
            doAccept();
        }
    });
}

void HttpServer::beginShutdown() {
    if (shutdown_started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    stopping_.store(true, std::memory_order_release);
    error_code ec;
    acceptor_.cancel(ec);
    acceptor_.close(ec);
    signals_.cancel(ec);
    logger_.info("http_shutdown_begin",
                 {{"grace_ms", std::to_string(shutdown_grace_.count())}});

    shutdown_timer_.expires_after(shutdown_grace_);
    shutdown_timer_.async_wait([this](error_code timer_ec) {
        if (timer_ec) {
            return;
        }
        closeSessions();

        // Give posted close operations one event-loop turn before stopping.
        shutdown_timer_.expires_after(kShutdownPollDelay);
        shutdown_timer_.async_wait([this](error_code) { io_.stop(); });
    });
}

void HttpServer::closeSessions() {
    std::vector<std::shared_ptr<Session>> live;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        live.reserve(sessions_.size());
        for (auto& weak : sessions_) {
            if (auto session = weak.lock()) {
                live.push_back(std::move(session));
            }
        }
        sessions_.clear();
    }
    for (auto& session : live) {
        asio::post(session->strand_, [session] { session->close(); });
    }
}

void HttpServer::connectionOpened() {
    const auto value = active_connections_.fetch_add(1, std::memory_order_acq_rel) + 1;
    metrics_.setActiveConnections(value);
}

void HttpServer::connectionClosed() {
    auto value = active_connections_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (value < 0) {
        value = 0;
        active_connections_.store(0, std::memory_order_release);
    }
    metrics_.setActiveConnections(value);
}

void HttpServer::rememberSession(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.push_back(session);
    if (sessions_.size() > kMaxTrackedSessions) {
        pruneSessionsLocked();
    }
}

void HttpServer::pruneSessionsLocked() {
    std::erase_if(sessions_, [](const auto& weak) { return weak.expired(); });
}

}  