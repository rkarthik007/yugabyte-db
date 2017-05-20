// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#ifndef YB_RPC_REACTOR_H_
#define YB_RPC_REACTOR_H_

#include <stdint.h>

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <ev++.h> // NOLINT

#include <boost/intrusive/list.hpp>
#include <boost/utility.hpp>

#include "yb/gutil/ref_counted.h"
#include "yb/rpc/connection.h"
#include "yb/util/thread.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/socket.h"
#include "yb/util/status.h"

namespace yb {
namespace rpc {

// When compiling on Mac OS X, use 'kqueue' instead of the default, 'select', for the event loop.
// Otherwise we run into problems because 'select' can't handle connections when more than 1024
// file descriptors are open by the process.
#if defined(__APPLE__)
constexpr unsigned int kDefaultLibEvFlags = ev::KQUEUE;
#else
constexpr unsigned int kDefaultLibEvFlags = ev::AUTO;
#endif

typedef std::list<ConnectionPtr> conn_list_t;

class DumpRunningRpcsRequestPB;
class DumpRunningRpcsResponsePB;
class Messenger;
class MessengerBuilder;
class Reactor;

// Simple metrics information from within a reactor.
struct ReactorMetrics {
  // Number of client RPC connections currently connected.
  int32_t num_client_connections_;
  // Number of server RPC connections currently connected.
  int32_t num_server_connections_;
};

// A task which can be enqueued to run on the reactor thread.
class ReactorTask : public std::enable_shared_from_this<ReactorTask> {
 public:
  ReactorTask();
  ReactorTask(const ReactorTask&) = delete;
  void operator=(const ReactorTask&) = delete;

  // Run the task. 'reactor' is guaranteed to be the current thread.
  virtual void Run(ReactorThread *reactor) = 0;

  // Abort the task, in the case that the reactor shut down before the
  // task could be processed. This may or may not run on the reactor thread
  // itself.
  // If this is run not on reactor thread, then reactor thread should be already shut down.
  //
  // The Reactor guarantees that the Reactor lock is free when this
  // method is called.
  virtual void Abort(const Status &abort_status) {}

  virtual ~ReactorTask();
};

template <class F>
class FunctorReactorTask : public ReactorTask {
 public:
  explicit FunctorReactorTask(const F& f) : f_(f) {}

  void Run(ReactorThread *reactor) override  {
    f_(reactor);
  }
 private:
  F f_;
};

template <class F>
std::shared_ptr<ReactorTask> MakeFunctorReactorTask(const F& f) {
  return std::make_shared<FunctorReactorTask<F>>(f);
}

template <class F, class Object>
class FunctorReactorTaskWithWeakPtr : public ReactorTask {
 public:
  explicit FunctorReactorTaskWithWeakPtr(const F& f, const std::weak_ptr<Object>& ptr)
      : f_(f), ptr_(ptr) {}

  void Run(ReactorThread *reactor) override  {
    auto shared_ptr = ptr_.lock();
    if (shared_ptr) {
      f_(reactor);
    }
  }
 private:
  F f_;
  std::weak_ptr<Object> ptr_;
};

template <class F, class Object>
std::shared_ptr<ReactorTask> MakeFunctorReactorTask(const F& f,
                                                    const std::weak_ptr<Object>& ptr) {
  return std::make_shared<FunctorReactorTaskWithWeakPtr<F, Object>>(f, ptr);
}

template <class F, class Object>
std::shared_ptr<ReactorTask> MakeFunctorReactorTask(const F& f,
                                                    const std::shared_ptr<Object>& ptr) {
  return std::make_shared<FunctorReactorTaskWithWeakPtr<F, Object>>(f, ptr);
}

// A ReactorTask that is scheduled to run at some point in the future.
//
// Semantically it works like RunFunctionTask with a few key differences:
// 1. The user function is called during Abort. Put another way, the
//    user function is _always_ invoked, even during reactor shutdown.
// 2. To differentiate between Abort and non-Abort, the user function
//    receives a Status as its first argument.
class DelayedTask : public ReactorTask {
 public:
  DelayedTask(std::function<void(const Status&)> func, MonoDelta when, int64_t id,
              const std::shared_ptr<Messenger> messenger);

  // Schedules the task for running later but doesn't actually run it yet.
  virtual void Run(ReactorThread* reactor) override;

  // Behaves like ReactorTask::Abort.
  virtual void Abort(const Status& abort_status) override;

  // Could be called from non-reactor thread even before reactor thread shutdown.
  void AbortTask(const Status& abort_status);

 private:
  // Set done_ to true if not set and return true. If done_ is already set, return false;
  bool MarkAsDone();

  // libev callback for when the registered timer fires.
  void TimerHandler(ev::timer& watcher, int revents); // NOLINT

  // User function to invoke when timer fires or when task is aborted.
  const std::function<void(const Status&)> func_;

  // Delay to apply to this task.
  const MonoDelta when_;

  // Link back to registering reactor thread.
  ReactorThread* thread_;

  // libev timer. Set when Run() is invoked.
  ev::timer timer_;

  // This task's id.
  const int64_t id_;

  std::shared_ptr<Messenger> messenger_;

  // Set to true whenever a Run or Abort methods are called.
  // Guarded by lock_.
  bool done_;

  typedef simple_spinlock LockType;
  mutable LockType lock_;
};

// A ReactorThread is a libev event handler thread which manages I/O
// on a list of sockets.
//
// All methods in this class are _only_ called from the reactor thread itself
// except where otherwise specified. New methods should DCHECK(IsCurrentThread())
// to ensure this.
class ReactorThread {
 public:
  friend class Connection;
  friend class YBConnection;
  friend class RedisConnection;

  // Client-side connection map.
  typedef std::unordered_map<const ConnectionId, ConnectionPtr,
                             ConnectionIdHash, ConnectionIdEqual> conn_map_t;

  ReactorThread(Reactor *reactor, const MessengerBuilder &bld);

  // This may be called from another thread.
  CHECKED_STATUS Init();

  // Add any connections on this reactor thread into the given status dump.
  // May be called from another thread.
  CHECKED_STATUS DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                         DumpRunningRpcsResponsePB* resp);

  // Block until the Reactor thread is shut down
  //
  // This must be called from another thread.
  void Shutdown();

  // This method is thread-safe.
  void WakeThread();

  // libev callback for handling async notifications in our epoll thread.
  void AsyncHandler(ev::async &watcher, int revents); // NOLINT

  // libev callback for handling timer events in our epoll thread.
  void TimerHandler(ev::timer &watcher, int revents); // NOLINT

  // Register an epoll timer watcher with our event loop.
  // Does not set a timeout or start it.
  void RegisterTimeout(ev::timer *watcher);

  // This may be called from another thread.
  const std::string &name() const;

  MonoTime cur_time() const;

  // This may be called from another thread.
  Reactor *reactor();

  // Return true if this reactor thread is the thread currently
  // running. Should be used in DCHECK assertions.
  bool IsCurrentThread() const;

  // Begin the process of connection negotiation.
  // Must be called from the reactor thread.
  // Deadline specifies latest time negotiation may complete before timeout.
  CHECKED_STATUS StartConnectionNegotiation(const ConnectionPtr& conn,
                                            const MonoTime& deadline);

  // Transition back from negotiating to processing requests.
  // Must be called from the reactor thread.
  void CompleteConnectionNegotiation(const ConnectionPtr& conn, const Status& status);

  // Queue a new call to be sent. If the reactor is already shut down, marks
  // the call as failed.
  void QueueOutboundCall(OutboundCallPtr call);

  // Collect metrics.
  // Must be called from the reactor thread.
  CHECKED_STATUS GetMetrics(ReactorMetrics *metrics);

  void Join() { thread_->Join(); }

 private:
  friend class AssignOutboundCallTask;
  friend class RegisterConnectionTask;
  friend class QueueServerEventTask;
  friend class DelayedTask;

  // Queues a server event on all the connections, such that every client receives it.
  CHECKED_STATUS QueueEventOnAllConnections(scoped_refptr<ServerEvent> server_event);

  // Run the main event loop of the reactor.
  void RunThread();

  // Find or create a new connection to the given remote.
  // If such a connection already exists, returns that, otherwise creates a new one.
  // May return a bad Status if the connect() call fails.
  // The resulting connection object is managed internally by the reactor thread.
  // Deadline specifies latest time allowed for initializing the connection.
  CHECKED_STATUS FindOrStartConnection(const ConnectionId &conn_id,
                                       const MonoTime &deadline,
                                       ConnectionPtr* conn);

  // Shut down the given connection, removing it from the connection tracking
  // structures of this reactor.
  //
  // The connection is not explicitly deleted -- shared_ptr reference counting
  // may hold on to the object after this, but callers should assume that it
  // _may_ be deleted by this call.
  void DestroyConnection(Connection *conn, const Status &conn_status);

  // Scan any open connections for idle ones that have been idle longer than
  // connection_keepalive_time_
  void ScanIdleConnections();

  // Create a new client socket (non-blocking, NODELAY)
  static CHECKED_STATUS CreateClientSocket(Socket *sock);

  // Initiate a new connection on the given socket, setting *in_progress
  // to true if the connection is still pending upon return.
  static CHECKED_STATUS StartConnect(Socket *sock, const Sockaddr &remote, bool *in_progress);

  // Assign a new outbound call to the appropriate connection object.
  // If this fails, the call is marked failed and completed.
  ConnectionPtr AssignOutboundCall(const OutboundCallPtr &call);

  // Register a new connection.
  void RegisterConnection(const ConnectionPtr& conn);

  // Actually perform shutdown of the thread, tearing down any connections,
  // etc. This is called from within the thread.
  void ShutdownInternal();

  void ProcessOutboundQueue();

  void CheckReadyToStop();

  scoped_refptr<yb::Thread> thread_;

  // our epoll object (or kqueue, etc).
  ev::dynamic_loop loop_;

  // Used by other threads to notify the reactor thread
  ev::async async_;

  // Handles the periodic timer.
  ev::timer timer_;

  // Scheduled (but not yet run) delayed tasks.
  //
  // Each task owns its own memory and must be freed by its TaskRun and
  // Abort members, provided it was allocated on the heap.
  std::set<std::shared_ptr<DelayedTask>> scheduled_tasks_;

  std::vector<std::shared_ptr<ReactorTask>> async_handler_tasks_;

  // The current monotonic time.  Updated every coarse_timer_granularity_.
  MonoTime cur_time_;

  // last time we did TCP timeouts.
  MonoTime last_unused_tcp_scan_;

  // Map of sockaddrs to Connection objects for outbound (client) connections.
  conn_map_t client_conns_;

  // List of current connections coming into the server.
  conn_list_t server_conns_;

  // List of connections that should be completed before we could stop this thread.
  conn_list_t waiting_conns_;

  Reactor *reactor_;

  // If a connection has been idle for this much time, it is torn down.
  const MonoDelta connection_keepalive_time_;

  // Scan for idle connections on this granularity.
  const MonoDelta coarse_timer_granularity_;

  simple_spinlock outbound_queue_lock_;
  bool closing_ = false;
  // We found that should shutdown, but not all connections are ready for it.
  bool stopping_ = false;
  std::vector<OutboundCallPtr> outbound_queue_;
  std::vector<OutboundCallPtr> processing_outbound_queue_;
  std::vector<ConnectionPtr> processing_connections_;
  std::shared_ptr<ReactorTask> process_outbound_queue_task_;
};

// A Reactor manages a ReactorThread
class Reactor {
 public:
  Reactor(const std::shared_ptr<Messenger>& messenger,
          int index,
          const MessengerBuilder &bld);
  CHECKED_STATUS Init();

  // Block until the Reactor is shut down
  void Shutdown();

  ~Reactor();

  const std::string &name() const;

  // Collect metrics about the reactor.
  CHECKED_STATUS GetMetrics(ReactorMetrics *metrics);

  // Add any connections on this reactor thread into the given status dump.
  CHECKED_STATUS DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                         DumpRunningRpcsResponsePB* resp);

  // Queues a server event on all the connections, such that every client receives it.
  void QueueEventOnAllConnections(scoped_refptr<ServerEvent> server_event);

  // Queue a new incoming connection. Takes ownership of the underlying fd from
  // 'socket', but not the Socket object itself.
  // If the reactor is already shut down, takes care of closing the socket.
  void RegisterInboundSocket(Socket *socket, const Sockaddr &remote);

  // Queue a new call to be sent. If the reactor is already shut down, marks
  // the call as failed.
  void QueueOutboundCall(OutboundCallPtr call) {
    thread_.QueueOutboundCall(std::move(call));
  }

  // Schedule the given task's Run() method to be called on the
  // reactor thread.
  // If the reactor shuts down before it is run, the Abort method will be
  // called.
  void ScheduleReactorTask(std::shared_ptr<ReactorTask> task);

  CHECKED_STATUS RunOnReactorThread(std::function<Status()>&& f);

  // If the Reactor is closing, returns false.
  // Otherwise, drains the pending_tasks_ queue into the provided list.
  bool DrainTaskQueue(std::vector<std::shared_ptr<ReactorTask>> *tasks);

  Messenger *messenger() const {
    return messenger_.get();
  }

  // Indicates whether the reactor is shutting down.
  //
  // This method is thread-safe.
  bool closing() const;

  // Is this reactor's thread the current thread?
  bool IsCurrentThread() const {
    return thread_.IsCurrentThread();
  }

  void Join() {
    thread_.Join();
  }

 private:
  friend class ReactorThread;
  typedef simple_spinlock LockType;
  mutable LockType lock_;

  // parent messenger
  std::shared_ptr<Messenger> messenger_;

  const std::string name_;

  // Whether the reactor is shutting down.
  // Guarded by lock_.
  bool closing_;

  const ConnectionType connection_type_;

  // Tasks to be run within the reactor thread.
  // Guarded by lock_.
  std::vector<std::shared_ptr<ReactorTask>> pending_tasks_;

  ReactorThread thread_;

  DISALLOW_COPY_AND_ASSIGN(Reactor);
};

}  // namespace rpc
}  // namespace yb

#endif // YB_RPC_REACTOR_H_
