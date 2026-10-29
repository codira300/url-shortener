#pragma once

#include "config/Config.h"
#include "http/HttpMessages.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace urlshort {

  class ThreadPool;
  class Metrics;
  class Logger;

  class HttpServer {

    public:
      using Handler = std::function<HttpResponse(const HttpRequest&)>;

      HttpServer(const HttpConfig& config, int shutdown_grace_ms, Handler handler, ThreadPool& workers,
                Metrics& metrics, Logger& logger);

      ~HttpServer();

      HttpServer(const HttpServer&) = delete;
      HttpServer& operator=(const HttpServer&) = delete;

      void run();
      void stop();

    private:
      class Session;

      void doAccept();
      void beginShutdown();
      void closeSessions();
      void connectionOpened();
      void connectionClosed();
      void rememberSession(const std::shared_ptr<Session>& session);
      void pruneSessionsLocked();

      friend class Session;

      HttpConfig config_;
      std::chrono::milliseconds shutdown_grace_;
      Handler handler_;
      ThreadPool& workers_;
      Metrics& metrics_;
      Logger& logger_;

      boost::asio::io_context io_;
      boost::asio::ip::tcp::acceptor acceptor_;
      boost::asio::signal_set signals_;
      boost::asio::steady_timer shutdown_timer_;

      std::atomic<bool> stopping_{false};
      std::atomic<bool> shutdown_started_{false};
      std::atomic<std::int64_t> active_connections_{0};

      std::mutex sessions_mutex_;
      std::vector<std::weak_ptr<Session>> sessions_;
  };

}  
