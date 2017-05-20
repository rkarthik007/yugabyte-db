//
// Copyright (c) YugaByte, Inc.
//

#include "yb/rpc/yb_rpc.h"

#include "yb/gutil/endian.h"

#include "yb/rpc/auth_store.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/negotiation.h"
#include "yb/rpc/reactor.h"
#include "yb/rpc/rpc_introspection.pb.h"
#include "yb/rpc/sasl_client.h"
#include "yb/rpc/sasl_server.h"
#include "yb/rpc/serialization.h"

#include "yb/util/size_literals.h"
#include "yb/util/debug/trace_event.h"

using yb::operator"" _MB;

DECLARE_bool(rpc_dump_all_traces);
DEFINE_int32(rpc_max_message_size, 8_MB,
             "The maximum size of a message of any RPC that the server will accept.");

using std::placeholders::_1;
DECLARE_int32(rpc_slow_query_threshold_ms);

namespace yb {
namespace rpc {

YBConnectionContext::YBConnectionContext() {}

YBConnectionContext::~YBConnectionContext() {}

void YBConnectionContext::RunNegotiation(ConnectionPtr connection, const MonoTime& deadline) {
  Negotiation::YBNegotiation(std::move(connection), this, deadline);
}

size_t YBConnectionContext::BufferLimit() {
  return FLAGS_rpc_max_message_size;
}

Status YBConnectionContext::ProcessCalls(const ConnectionPtr& connection,
                                         Slice slice,
                                         size_t* consumed) {
  auto pos = slice.data();
  const auto end = slice.end();
  while (end - pos >= kMsgLengthPrefixLength) {
    const size_t data_length = NetworkByteOrder::Load32(pos);
    const size_t total_length = data_length + kMsgLengthPrefixLength;
    if (total_length > FLAGS_rpc_max_message_size) {
      return STATUS(NetworkError,
          strings::Substitute("The frame had a length of $0, but we only support "
                              "messages up to $1 bytes long.",
                              total_length,
                              FLAGS_rpc_max_message_size));
    }
    auto stop = pos + total_length;
    if (stop > end) {
      break;
    }
    pos += kMsgLengthPrefixLength;
    const auto status = HandleCall(connection, Slice(pos, stop - pos));
    if (!status.ok()) {
      return status;
    }

    pos = stop;
  }
  *consumed = pos - slice.data();
  return Status::OK();
}

Status YBConnectionContext::HandleCall(const ConnectionPtr& connection, Slice call_data) {
  const auto direction = connection->direction();
  switch (direction) {
    case ConnectionDirection::CLIENT:
      return connection->HandleCallResponse(call_data);
    case ConnectionDirection::SERVER:
      return HandleInboundCall(connection, call_data);
  }
  LOG(FATAL) << "Invalid direction: " << direction;
}

void YBConnectionContext::DumpPB(const DumpRunningRpcsRequestPB& req,
                                 RpcConnectionPB* resp) {
  for (const auto &entry : calls_being_handled_) {
    entry.second->DumpPB(req, resp->add_calls_in_flight());
  }
}

bool YBConnectionContext::Idle() {
  return calls_being_handled_.empty();
}

Status YBConnectionContext::InitSaslClient(Connection* connection) {
  sasl_client_.reset(new SaslClient(kSaslAppName, connection->socket()->GetFd()));
  RETURN_NOT_OK(sasl_client().Init(kSaslProtoName));
  RETURN_NOT_OK(sasl_client().EnableAnonymous());
  const auto& credentials = connection->user_credentials();
  RETURN_NOT_OK(sasl_client().EnablePlain(credentials.real_user(),
      credentials.password()));
  return Status::OK();
}

Status YBConnectionContext::InitSaslServer(Connection* connection) {
  sasl_server_.reset(new SaslServer(kSaslAppName, connection->socket()->GetFd()));
  // TODO: Do necessary configuration plumbing to enable user authentication.
  // Right now we just enable PLAIN with a "dummy" auth store, which allows everyone in.
  RETURN_NOT_OK(sasl_server().Init(kSaslProtoName));
  gscoped_ptr<AuthStore> auth_store(new DummyAuthStore());
  RETURN_NOT_OK(sasl_server().EnablePlain(auth_store.Pass()));
  return Status::OK();
}

Status YBConnectionContext::HandleInboundCall(const ConnectionPtr& connection, Slice call_data) {
  auto reactor_thread = connection->reactor_thread();
  DCHECK(reactor_thread->IsCurrentThread());

  auto call_processed_listener = std::bind(&YBConnectionContext::EraseCall, this, _1);
  YBInboundCall * call;
  InboundCallPtr call_ptr(call = new YBInboundCall(connection, call_processed_listener));

  Status s = call->ParseFrom(call_data);
  if (!s.ok()) {
    return s;
  }

  // call_id exists only for YB. Not for Redis.
  auto id = call->call_id();
  if (!InsertIfNotPresent(&calls_being_handled_, id, call)) {
    LOG(WARNING) << connection->ToString() << ": received call ID " << call->call_id()
                 << " but was already processing this ID! Ignoring";
    return STATUS_SUBSTITUTE(NetworkError, "Received duplicate call id: $0", call->call_id());
  }

  reactor_thread->reactor()->messenger()->QueueInboundCall(call_ptr);

  return Status::OK();
}

void YBConnectionContext::EraseCall(InboundCall* call) {
  auto* yb_call = down_cast<YBInboundCall*>(call);
  // Remove the call from the map.
  InboundCall* call_from_map = EraseKeyReturnValuePtr(
      &calls_being_handled_, yb_call->call_id());
  DCHECK_EQ(call_from_map, call);
}

YBInboundCall::YBInboundCall(ConnectionPtr conn, CallProcessedListener call_processed_listener)
    : InboundCall(std::move(conn), std::move(call_processed_listener)) {}

MonoTime YBInboundCall::GetClientDeadline() const {
  if (!header_.has_timeout_millis() || header_.timeout_millis() == 0) {
    return MonoTime::Max();
  }
  MonoTime deadline = timing_.time_received;
  deadline.AddDelta(MonoDelta::FromMilliseconds(header_.timeout_millis()));
  return deadline;
}

Status YBInboundCall::ParseFrom(Slice source) {
  TRACE_EVENT_FLOW_BEGIN0("rpc", "YBInboundCall", this);
  TRACE_EVENT0("rpc", "YBInboundCall::ParseFrom");

  request_data_.assign(source.data(), source.end());
  source = Slice(request_data_.data(), request_data_.size());
  RETURN_NOT_OK(serialization::ParseYBMessage(source, &header_, &serialized_request_));

  // Adopt the service/method info from the header as soon as it's available.
  if (PREDICT_FALSE(!header_.has_remote_method())) {
    return STATUS(Corruption, "Non-connection context request header must specify remote_method");
  }
  if (PREDICT_FALSE(!header_.remote_method().IsInitialized())) {
    return STATUS(Corruption, "remote_method in request header is not initialized",
        header_.remote_method().InitializationErrorString());
  }
  remote_method_.FromPB(header_.remote_method());

  return Status::OK();
}

Status YBInboundCall::SerializeResponseBuffer(const google::protobuf::MessageLite& response,
                                              bool is_success) {
  using serialization::SerializeMessage;
  using serialization::SerializeHeader;

  uint32_t protobuf_msg_size = response.ByteSize();

  ResponseHeader resp_hdr;
  resp_hdr.set_call_id(header_.call_id());
  resp_hdr.set_is_error(!is_success);
  uint32_t absolute_sidecar_offset = protobuf_msg_size;
  for (auto& car : sidecars_) {
    resp_hdr.add_sidecar_offsets(absolute_sidecar_offset);
    absolute_sidecar_offset += car.size();
  }

  int additional_size = absolute_sidecar_offset - protobuf_msg_size;

  size_t message_size = 0;
  auto status = SerializeMessage(response,
                                 /* param_buf */ nullptr,
                                 additional_size,
                                 /* use_cached_size */ true,
                                 /* offset */ 0,
                                 &message_size);
  if (!status.ok()) {
    return status;
  }
  size_t header_size = 0;
  status = SerializeHeader(resp_hdr,
                           message_size + additional_size,
                           &response_buf_,
                           message_size,
                           &header_size);
  if (!status.ok()) {
    return status;
  }
  return SerializeMessage(response,
                          &response_buf_,
                          additional_size,
                          /* use_cached_size */ true,
                          header_size);
}

string YBInboundCall::ToString() const {
  return strings::Substitute("Call $0 from $1 (request call id $2)",
      remote_method_.ToString(),
      remote_address().ToString(),
      header_.call_id());
}

void YBInboundCall::DumpPB(const DumpRunningRpcsRequestPB& req,
                           RpcCallInProgressPB* resp) {
  resp->mutable_header()->CopyFrom(header_);
  if (req.include_traces() && trace_) {
    resp->set_trace_buffer(trace_->DumpToString(true));
  }
  resp->set_micros_elapsed(MonoTime::Now(MonoTime::FINE).GetDeltaSince(timing_.time_received)
      .ToMicroseconds());
}

void YBInboundCall::LogTrace() const {
  MonoTime now = MonoTime::Now(MonoTime::FINE);
  int total_time = now.GetDeltaSince(timing_.time_received).ToMilliseconds();

  if (header_.has_timeout_millis() && header_.timeout_millis() > 0) {
    double log_threshold = header_.timeout_millis() * 0.75f;
    if (total_time > log_threshold) {
      // TODO: consider pushing this onto another thread since it may be slow.
      // The traces may also be too large to fit in a log message.
      LOG(WARNING) << ToString() << " took " << total_time << "ms (client timeout "
                   << header_.timeout_millis() << "ms).";
      std::string s = trace_->DumpToString(true);
      if (!s.empty()) {
        LOG(WARNING) << "Trace:\n" << s;
      }
      return;
    }
  }

  if (PREDICT_FALSE(
          FLAGS_rpc_dump_all_traces ||
          total_time > FLAGS_rpc_slow_query_threshold_ms)) {
    LOG(INFO) << ToString() << " took " << total_time << "ms. Trace:";
    trace_->Dump(&LOG(INFO), true);
  }
}

void YBInboundCall::Serialize(std::deque<util::RefCntBuffer>* output) const {
  TRACE_EVENT0("rpc", "YBInboundCall::Serialize");
  CHECK_GT(response_buf_.size(), 0);
  output->push_back(response_buf_);
  for (auto& car : sidecars_) {
    output->push_back(car);
  }
}

} // namespace rpc
} // namespace yb
