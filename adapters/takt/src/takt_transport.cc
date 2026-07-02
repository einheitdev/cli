/// @file takt_transport.cc
/// @brief ZMQ transport for takt-service. Translates
/// framework Request/Response to/from takt's JSON protocol.
// Copyright (c) 2026 Einheit Networks

#include "takt_transport.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::adapters::takt {
namespace {

using cli::Error;
using cli::transport::TransportError;

auto MakeError(TransportError code, std::string msg)
    -> Error<TransportError> {
  return Error<TransportError>{code, std::move(msg)};
}

/// Map framework wire_command to takt-service cmd name.
auto MapCommand(
    const cli::protocol::Request &req)
    -> nlohmann::json {
  nlohmann::json j;
  j["cmd"] = req.command;
  if (!req.args.empty()) {
    if (req.command == "list_runs" ||
        req.command == "get_pipeline" ||
        req.command == "trigger_run") {
      j["workspace"] = req.args[0];
    } else if (req.command == "get_run_detail" ||
               req.command == "cancel_run") {
      try {
        j["run_id"] = std::stoi(req.args[0]);
      } catch (...) {
        j["run_id"] = req.args[0];
      }
    } else if (req.command == "get_workspace") {
      j["workspace"] = req.args[0];
    } else if (req.command == "claim_target") {
      j["name"] = req.args[0];
      if (req.args.size() > 1)
        j["workspace"] = req.args[1];
    } else if (req.command == "release_target" ||
               req.command == "target_up" ||
               req.command == "target_down") {
      j["name"] = req.args[0];
    } else if (req.command == "create_workspace") {
      j["name"] = req.args[0];
      if (req.args.size() > 1) {
        nlohmann::json repos = nlohmann::json::array();
        for (std::size_t i = 1; i < req.args.size();
             ++i) {
          repos.push_back(req.args[i]);
        }
        j["repos"] = repos;
      }
    } else if (req.command == "delete_workspace") {
      j["name"] = req.args[0];
    }
  }
  for (const auto &[k, v] : req.flags) {
    j[k] = v;
  }
  return j;
}

/// Parse takt-service JSON response into framework
/// Response struct.
auto ParseResponse(const std::string &json_str,
                   const std::string &req_id)
    -> cli::protocol::Response {
  cli::protocol::Response resp;
  resp.id = req_id;
  try {
    auto j = nlohmann::json::parse(json_str);
    auto status = j.value("status", "ok");
    if (status == "error") {
      resp.status = cli::protocol::ResponseStatus::Error;
      cli::protocol::ResponseError err;
      err.code = "takt_error";
      err.message = j.value("message", "unknown error");
      resp.error = std::move(err);
    } else {
      resp.status = cli::protocol::ResponseStatus::Ok;
      auto data = j.value("data", j);
      auto s = data.dump();
      resp.data.assign(s.begin(), s.end());
    }
  } catch (const std::exception &e) {
    resp.status = cli::protocol::ResponseStatus::Error;
    resp.error = cli::protocol::ResponseError{
        "parse_error", std::format("bad JSON: {}", e.what()),
        ""};
  }
  return resp;
}

class TaktTransport final
    : public cli::transport::Transport {
 public:
  explicit TaktTransport(TaktTransportConfig cfg)
      : cfg_(std::move(cfg)),
        ctx_(1),
        ctrl_(ctx_, zmq::socket_type::dealer),
        sub_(ctx_, zmq::socket_type::sub) {}

  ~TaktTransport() override {
    sub_stop_ = true;
    if (sub_thread_.joinable()) sub_thread_.join();
    try { ctrl_.close(); } catch (...) {}
    try { sub_.close(); } catch (...) {}
  }

  auto Connect()
      -> std::expected<void,
                       Error<TransportError>> override {
    try {
      ctrl_.connect(cfg_.control_endpoint);
      sub_.connect(cfg_.event_endpoint);
      connected_ = true;
      return {};
    } catch (const zmq::error_t &e) {
      return std::unexpected(MakeError(
          TransportError::ConnectFailed, e.what()));
    }
  }

  auto Disconnect() -> void override {
    sub_stop_ = true;
    if (sub_thread_.joinable()) sub_thread_.join();
    connected_ = false;
  }

  auto SendRequest(
      const cli::protocol::Request &req,
      std::chrono::milliseconds timeout)
      -> std::expected<cli::protocol::Response,
                       Error<TransportError>> override {
    if (!connected_) {
      return std::unexpected(MakeError(
          TransportError::InvalidState,
          "not connected"));
    }
    auto payload = MapCommand(req).dump();
    try {
      std::lock_guard<std::mutex> lk(ctrl_mu_);
      zmq::message_t empty;
      ctrl_.send(empty, zmq::send_flags::sndmore);
      zmq::message_t frame(payload.data(),
                           payload.size());
      ctrl_.send(frame, zmq::send_flags::none);
      zmq::pollitem_t item{
          static_cast<void *>(ctrl_), 0,
          ZMQ_POLLIN, 0};
      auto ms = static_cast<long>(timeout.count());
      int rc = zmq::poll(
          &item, 1,
          std::chrono::milliseconds{ms});
      if (rc <= 0) {
        return std::unexpected(MakeError(
            TransportError::Timeout, "timed out"));
      }
      zmq::message_t delim;
      (void)ctrl_.recv(delim, zmq::recv_flags::none);
      zmq::message_t reply;
      auto got = ctrl_.recv(
          reply, zmq::recv_flags::none);
      if (!got) {
        return std::unexpected(MakeError(
            TransportError::ReceiveFailed,
            "empty reply"));
      }
      auto text = std::string(
          static_cast<const char *>(reply.data()),
          reply.size());
      return ParseResponse(text, req.id);
    } catch (const zmq::error_t &e) {
      return std::unexpected(MakeError(
          TransportError::SendFailed, e.what()));
    }
  }

  auto Subscribe(
      const std::string &topic_prefix,
      cli::transport::EventCallback cb)
      -> std::expected<void,
                       Error<TransportError>> override {
    try {
      sub_.set(zmq::sockopt::subscribe, topic_prefix);
    } catch (const zmq::error_t &e) {
      return std::unexpected(MakeError(
          TransportError::InvalidState, e.what()));
    }
    if (!sub_thread_.joinable()) {
      sub_thread_ = std::thread(
          [this, cb = std::move(cb)]() {
            RunSubLoop(cb);
          });
    }
    return {};
  }

  auto Unsubscribe(const std::string &topic_prefix)
      -> std::expected<void,
                       Error<TransportError>> override {
    try {
      sub_.set(zmq::sockopt::unsubscribe,
               topic_prefix);
      return {};
    } catch (const zmq::error_t &e) {
      return std::unexpected(MakeError(
          TransportError::InvalidState, e.what()));
    }
  }

 private:
  void RunSubLoop(cli::transport::EventCallback cb) {
    while (!sub_stop_) {
      zmq::pollitem_t item{
          static_cast<void *>(sub_), 0,
          ZMQ_POLLIN, 0};
      int rc = zmq::poll(
          &item, 1, std::chrono::milliseconds{500});
      if (rc <= 0) continue;
      zmq::message_t topic_frame;
      auto got = sub_.recv(
          topic_frame, zmq::recv_flags::none);
      if (!got) continue;
      auto topic = std::string(
          static_cast<const char *>(topic_frame.data()),
          topic_frame.size());
      zmq::message_t body_frame;
      (void)sub_.recv(body_frame, zmq::recv_flags::none);
      auto body = std::string(
          static_cast<const char *>(body_frame.data()),
          body_frame.size());
      cli::protocol::Event ev;
      ev.topic = topic;
      ev.data.assign(body.begin(), body.end());
      cb(ev);
    }
  }

  TaktTransportConfig cfg_;
  zmq::context_t ctx_;
  zmq::socket_t ctrl_;
  zmq::socket_t sub_;
  std::mutex ctrl_mu_;
  bool connected_ = false;
  std::atomic<bool> sub_stop_{false};
  std::thread sub_thread_;
};

}  // namespace

auto MakeTaktTransport(TaktTransportConfig cfg)
    -> std::expected<
        std::unique_ptr<cli::transport::Transport>,
        Error<TransportError>> {
  return std::make_unique<TaktTransport>(
      std::move(cfg));
}

}  // namespace einheit::adapters::takt
