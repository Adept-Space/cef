// Copyright (c) 2020 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/ceftests/test_server.h"

#include <vector>

#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

namespace test_server {

// Must use a different port than server_unittest.cc.
const char kServerAddress[] = "127.0.0.1";
const uint16 kServerPort = 8098;
const char kServerScheme[] = "http";
const char kServerOrigin[] = "http://127.0.0.1:8098";

namespace {

class ServerManager;
ServerManager* g_manager = nullptr;

// True if Stop() has been called.
bool g_stopping = false;

// Created on the UI thread and called on the dedicated server thread.
class ServerHandler : public CefServerHandler {
 public:
  ServerHandler() {
    CefServer::CreateServer(kServerAddress, kServerPort, 10, this);
  }

  ~ServerHandler() override {
    DCHECK(!server_);
    NotifyServerHandlerDeleted();
  }

  void Shutdown() { server_->Shutdown(); }

 protected:
  // CefServerHandler methods:
  void OnServerCreated(CefRefPtr<CefServer> server) override {
    server_ = server;
    NotifyServerCreated(kServerOrigin);
  }

  void OnServerDestroyed(CefRefPtr<CefServer> server) override {
    server_ = nullptr;
    NotifyServerDestroyed();
  }

  void OnClientConnected(CefRefPtr<CefServer> server,
                         int connection_id) override {
    DCHECK(server->HasConnection());
    DCHECK(server->IsValidConnection(connection_id));
    NotifyClientConnected(server, connection_id);
  }

  void OnClientDisconnected(CefRefPtr<CefServer> server,
                            int connection_id) override {
    DCHECK(!server->IsValidConnection(connection_id));
    NotifyClientDisconnected(server, connection_id);
  }

  void OnHttpRequest(CefRefPtr<CefServer> server,
                     int connection_id,
                     const CefString& client_address,
                     CefRefPtr<CefRequest> request) override {
    NotifyHttpRequest(server, connection_id, client_address, request);
  }

  void OnWebSocketRequest(CefRefPtr<CefServer> server,
                          int connection_id,
                          const CefString& client_address,
                          CefRefPtr<CefRequest> request,
                          CefRefPtr<CefCallback> callback) override {}
  void OnWebSocketConnected(CefRefPtr<CefServer> server,
                            int connection_id) override {}
  void OnWebSocketMessage(CefRefPtr<CefServer> server,
                          int connection_id,
                          const void* data,
                          size_t data_size) override {}

 private:
  static void NotifyServerCreated(const std::string& server_origin);
  static void NotifyServerDestroyed();
  static void NotifyServerHandlerDeleted();
  static void NotifyClientConnected(CefRefPtr<CefServer> server,
                                    int connection_id);
  static void NotifyClientDisconnected(CefRefPtr<CefServer> server,
                                       int connection_id);
  static void NotifyHttpRequest(CefRefPtr<CefServer> server,
                                int connection_id,
                                const CefString& client_address,
                                CefRefPtr<CefRequest> request);

  CefRefPtr<CefServer> server_;

  IMPLEMENT_REFCOUNTING(ServerHandler);
  DISALLOW_COPY_AND_ASSIGN(ServerHandler);
};

// Only accessed on the UI thread. Deletes itself after the server is stopped.
class ServerManager {
 public:
  ServerManager() {
    CEF_REQUIRE_UI_THREAD();
    DCHECK(!g_manager);
    g_manager = this;
  }

  ~ServerManager() {
    CEF_REQUIRE_UI_THREAD();
    DCHECK(observer_list_.empty());
    DCHECK(start_callback_list_.empty());
    DCHECK(stop_callback_.is_null());

    g_manager = nullptr;
  }

  void Start(const StartDoneCallback& callback) {
    CEF_REQUIRE_UI_THREAD();
    if (!origin_.empty()) {
      // The server is already running.
      callback.Run(origin_);
      return;
    }

    // If tests run in parallel, and the server is starting, then there may be
    // multiple pending callbacks.
    start_callback_list_.push_back(callback);

    // Only create the handler a single time.
    if (!handler_) {
      handler_ = new ServerHandler();
    }
  }

  void Stop(const DoneCallback& callback) {
    CEF_REQUIRE_UI_THREAD();
    if (!handler_) {
      // The server is not currently running.
      callback.Run();
      return;
    }

    // Only 1 stop callback supported.
    DCHECK(stop_callback_.is_null());
    stop_callback_ = callback;

    handler_->Shutdown();
  }

  void AddObserver(Observer* observer) {
    CEF_REQUIRE_UI_THREAD();
    observer_list_.push_back(observer);
  }

  void RemoveObserver(Observer* observer) {
    CEF_REQUIRE_UI_THREAD();
    bool found = false;
    ObserverList::iterator it = observer_list_.begin();
    for (; it != observer_list_.end(); ++it) {
      if (*it == observer) {
        observer_list_.erase(it);
        found = true;
        break;
      }
    }
    DCHECK(found);
  }

  void NotifyServerCreated(const std::string& server_origin) {
    CEF_REQUIRE_UI_THREAD();

    DCHECK(origin_.empty());
    origin_ = server_origin;

    StartDoneCallbackList::const_iterator it = start_callback_list_.begin();
    for (; it != start_callback_list_.end(); ++it) {
      (*it).Run(origin_);
    }
    start_callback_list_.clear();
  }

  void NotifyServerDestroyed() {
    CEF_REQUIRE_UI_THREAD();

    origin_.clear();
    handler_ = nullptr;
  }

  // All server-related objects have been torn down.
  void NotifyServerHandlerDeleted() {
    CEF_REQUIRE_UI_THREAD();

    DCHECK(!stop_callback_.is_null());
    stop_callback_.Run();
    stop_callback_.Reset();

    delete this;
  }

  void NotifyClientConnected(CefRefPtr<CefServer> server, int connection_id) {
    CEF_REQUIRE_UI_THREAD();

    DCHECK(!observer_list_.empty());

    // Use a copy in case |observer_list_| is modified during iteration.
    ObserverList list = observer_list_;

    ObserverList::const_iterator it = list.begin();
    for (; it != list.end(); ++it) {
      if ((*it)->OnClientConnected(server, connection_id)) {
        break;
      }
    }
  }

  void NotifyClientDisconnected(CefRefPtr<CefServer> server,
                                int connection_id) {
    CEF_REQUIRE_UI_THREAD();

    DCHECK(!observer_list_.empty());

    // Use a copy in case |observer_list_| is modified during iteration.
    ObserverList list = observer_list_;

    ObserverList::const_iterator it = list.begin();
    for (; it != list.end(); ++it) {
      if ((*it)->OnClientDisconnected(server, connection_id)) {
        break;
      }
    }
  }

  void NotifyHttpRequest(CefRefPtr<CefServer> server,
                         int connection_id,
                         const CefString& client_address,
                         CefRefPtr<CefRequest> request) {
    CEF_REQUIRE_UI_THREAD();

    DCHECK(!observer_list_.empty());

    // Use a copy in case |observer_list_| is modified during iteration.
    ObserverList list = observer_list_;

    ObserverList::const_iterator it = list.begin();
    for (; it != list.end(); ++it) {
      if ((*it)->OnHttpRequest(server, connection_id, client_address,
                               request)) {
        break;
      }
    }
  }

 private:
  CefRefPtr<ServerHandler> handler_;
  std::string origin_;

  typedef std::vector<StartDoneCallback> StartDoneCallbackList;
  StartDoneCallbackList start_callback_list_;

  DoneCallback stop_callback_;

  typedef std::vector<Observer*> ObserverList;
  ObserverList observer_list_;

  DISALLOW_COPY_AND_ASSIGN(ServerManager);
};

ServerManager* GetServerManager() {
  return g_manager;
}

ServerManager* GetOrCreateServerManager() {
  if (!g_manager) {
    new ServerManager();
    DCHECK(g_manager);
  }
  return g_manager;
}

// static
void ServerHandler::NotifyServerCreated(const std::string& server_origin) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI,
                base::Bind(ServerHandler::NotifyServerCreated, server_origin));
    return;
  }

  GetServerManager()->NotifyServerCreated(server_origin);
}

// static
void ServerHandler::NotifyServerDestroyed() {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::Bind(ServerHandler::NotifyServerDestroyed));
    return;
  }

  GetServerManager()->NotifyServerDestroyed();
}

// static
void ServerHandler::NotifyServerHandlerDeleted() {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::Bind(ServerHandler::NotifyServerHandlerDeleted));
    return;
  }

  GetServerManager()->NotifyServerHandlerDeleted();
}

// static
void ServerHandler::NotifyClientConnected(CefRefPtr<CefServer> server,
                                          int connection_id) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::Bind(ServerHandler::NotifyClientConnected, server,
                                   connection_id));
    return;
  }

  GetServerManager()->NotifyClientConnected(server, connection_id);
}

// static
void ServerHandler::NotifyClientDisconnected(CefRefPtr<CefServer> server,
                                             int connection_id) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::Bind(ServerHandler::NotifyClientDisconnected,
                                   server, connection_id));
    return;
  }

  GetServerManager()->NotifyClientDisconnected(server, connection_id);
}

// static
void ServerHandler::NotifyHttpRequest(CefRefPtr<CefServer> server,
                                      int connection_id,
                                      const CefString& client_address,
                                      CefRefPtr<CefRequest> request) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::Bind(ServerHandler::NotifyHttpRequest, server,
                                   connection_id, client_address, request));
    return;
  }

  GetServerManager()->NotifyHttpRequest(server, connection_id, client_address,
                                        request);
}

// May be created on any thread but will be destroyed on the UI thread.
class ObserverRegistration : public CefRegistration {
 public:
  explicit ObserverRegistration(Observer* const observer)
      : observer_(observer) {
    DCHECK(observer_);
  }

  ~ObserverRegistration() override {
    CEF_REQUIRE_UI_THREAD();

    ServerManager* manager = GetServerManager();
    if (manager) {
      manager->RemoveObserver(observer_);
      observer_->OnUnregistered();
    }
  }

  void Initialize() {
    CEF_REQUIRE_UI_THREAD();
    GetOrCreateServerManager()->AddObserver(observer_);
    observer_->OnRegistered();
  }

 private:
  Observer* const observer_;

  IMPLEMENT_REFCOUNTING_DELETE_ON_UIT(ObserverRegistration);
  DISALLOW_COPY_AND_ASSIGN(ObserverRegistration);
};

void InitializeRegistration(CefRefPtr<ObserverRegistration> registration,
                            const DoneCallback& callback) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI,
                base::Bind(InitializeRegistration, registration, callback));
    return;
  }

  DCHECK(!g_stopping);

  registration->Initialize();
  if (!callback.is_null())
    callback.Run();
}

}  // namespace

void Start(const StartDoneCallback& callback) {
  DCHECK(!callback.is_null());
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::Bind(Start, callback));
    return;
  }

  DCHECK(!g_stopping);

  GetOrCreateServerManager()->Start(callback);
}

void Stop(const DoneCallback& callback) {
  DCHECK(!callback.is_null());
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::Bind(Stop, callback));
    return;
  }

  // Stop will be called one time on test framework shutdown.
  DCHECK(!g_stopping);
  g_stopping = true;

  ServerManager* manager = GetServerManager();
  if (manager) {
    manager->Stop(callback);
  } else {
    callback.Run();
  }
}

CefRefPtr<CefRegistration> AddObserver(Observer* observer,
                                       const DoneCallback& callback) {
  DCHECK(observer);
  CefRefPtr<ObserverRegistration> registration =
      new ObserverRegistration(observer);
  InitializeRegistration(registration, callback);
  return registration.get();
}

CefRefPtr<CefRegistration> AddObserverAndStart(
    Observer* observer,
    const StartDoneCallback& callback) {
  return AddObserver(observer, base::Bind(Start, callback));
}

void SendResponse(CefRefPtr<CefServer> server,
                  int connection_id,
                  CefRefPtr<CefResponse> response,
                  const std::string& response_data) {
  const int response_code = response->GetStatus();
  const CefString& content_type = response->GetMimeType();
  int64 content_length = static_cast<int64>(response_data.size());

  CefResponse::HeaderMap extra_headers;
  response->GetHeaderMap(extra_headers);

  server->SendHttpResponse(connection_id, response_code, content_type,
                           content_length, extra_headers);

  if (content_length != 0) {
    server->SendRawData(connection_id, response_data.data(),
                        response_data.size());
    server->CloseConnection(connection_id);
  }
}

// ObserverHelper

ObserverHelper::ObserverHelper() : weak_ptr_factory_(this) {
  CEF_REQUIRE_UI_THREAD();
}

ObserverHelper::~ObserverHelper() {
  DCHECK(state_ == State::NONE);
}

void ObserverHelper::Initialize() {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(state_ == State::NONE);
  state_ = State::INITIALIZING;
  registration_ = AddObserverAndStart(
      this,
      base::Bind(&ObserverHelper::OnStartDone, weak_ptr_factory_.GetWeakPtr()));
}

void ObserverHelper::Shutdown() {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(state_ == State::INITIALIZED);
  state_ = State::SHUTTINGDOWN;
  registration_ = nullptr;
}

void ObserverHelper::OnStartDone(const std::string& server_origin) {
  DCHECK(state_ == State::INITIALIZING);
  state_ = State::INITIALIZED;
  OnInitialized(server_origin);
}

void ObserverHelper::OnRegistered() {
  DCHECK(state_ == State::INITIALIZING);
}

void ObserverHelper::OnUnregistered() {
  DCHECK(state_ == State::SHUTTINGDOWN);
  state_ = State::NONE;
  OnShutdown();
}

}  // namespace test_server