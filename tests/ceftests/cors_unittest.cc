// Copyright (c) 2020 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include <algorithm>
#include <vector>

#include "include/base/cef_bind.h"
#include "include/cef_callback.h"
#include "include/cef_origin_whitelist.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_closure_task.h"
#include "tests/ceftests/routing_test_handler.h"
#include "tests/ceftests/test_request.h"
#include "tests/ceftests/test_server.h"
#include "tests/ceftests/test_util.h"

namespace {

const char kMimeTypeHtml[] = "text/html";
const char kMimeTypeText[] = "text/plain";

const char kDefaultHtml[] = "<html><body>TEST</body></html>";
const char kDefaultText[] = "TEST";

const char kSuccessMsg[] = "CorsTestHandler.Success";
const char kFailureMsg[] = "CorsTestHandler.Failure";

// Source that will handle the request.
enum class HandlerType {
  SERVER,
  HTTP_SCHEME,
  CUSTOM_STANDARD_SCHEME,
  CUSTOM_NONSTANDARD_SCHEME,
};

std::string GetOrigin(HandlerType handler) {
  switch (handler) {
    case HandlerType::SERVER:
      return test_server::kServerOrigin;
    case HandlerType::HTTP_SCHEME:
      return "http://corstest.com";
    case HandlerType::CUSTOM_STANDARD_SCHEME:
      // Standard scheme that is CORS and fetch enabled.
      // Registered in scheme_handler_unittest.cc.
      return "customstdfetch://corstest";
    case HandlerType::CUSTOM_NONSTANDARD_SCHEME:
      // Non-sandard scheme that is not CORS or fetch enabled.
      // Registered in scheme_handler_unittest.cc.
      return "customnonstd:corstest";
  }
  NOTREACHED();
  return std::string();
}

std::string GetPathURL(HandlerType handler, const std::string& path) {
  return GetOrigin(handler) + path;
}

struct Resource {
  // Uniquely identifies the resource.
  HandlerType handler = HandlerType::SERVER;
  std::string path;

  // Response information that will be returned.
  CefRefPtr<CefResponse> response;
  std::string response_data;

  // Expected error code in OnLoadError.
  cef_errorcode_t expected_error_code = ERR_NONE;

  // Expected number of responses.
  int expected_response_ct = 1;

  // Expected number of OnQuery calls.
  int expected_success_query_ct = 0;
  int expected_failure_query_ct = 0;

  // Actual number of responses.
  int response_ct = 0;

  // Actual number of OnQuery calls.
  int success_query_ct = 0;
  int failure_query_ct = 0;

  Resource() {}
  Resource(HandlerType request_handler,
           const std::string& request_path,
           const std::string& mime_type = kMimeTypeHtml,
           const std::string& data = kDefaultHtml,
           int status = 200) {
    Init(request_handler, request_path, mime_type, data, status);
  }

  // Perform basic initialization.
  void Init(HandlerType request_handler,
            const std::string& request_path,
            const std::string& mime_type = kMimeTypeHtml,
            const std::string& data = kDefaultHtml,
            int status = 200) {
    handler = request_handler;
    path = request_path;
    response_data = data;
    response = CefResponse::Create();
    response->SetMimeType(mime_type);
    response->SetStatus(status);
  }

  // Validate expected initial state.
  void Validate() const {
    DCHECK(!path.empty());
    DCHECK(response);
    DCHECK(!response->GetMimeType().empty());
    DCHECK_EQ(0, response_ct);
    DCHECK_GE(expected_response_ct, 0);
  }

  std::string GetPathURL() const { return ::GetPathURL(handler, path); }

  // Returns true if all expectations have been met.
  bool IsDone() const {
    return response_ct == expected_response_ct &&
           success_query_ct == expected_success_query_ct &&
           failure_query_ct == expected_failure_query_ct;
  }

  void AssertDone() const {
    EXPECT_EQ(expected_response_ct, response_ct) << GetPathURL();
    EXPECT_EQ(expected_success_query_ct, success_query_ct) << GetPathURL();
    EXPECT_EQ(expected_failure_query_ct, failure_query_ct) << GetPathURL();
  }

  // Optionally override to verify request contents.
  virtual bool VerifyRequest(CefRefPtr<CefRequest> request) const {
    return true;
  }
};

struct TestSetup {
  // Available resources.
  typedef std::vector<Resource*> ResourceList;
  ResourceList resources;

  // Used for testing received console messages.
  std::vector<std::string> console_messages;

  void AddResource(Resource* resource) {
    DCHECK(resource);
    resource->Validate();
    resources.push_back(resource);
  }

  void AddConsoleMessage(const std::string& message) {
    DCHECK(!message.empty());
    console_messages.push_back(message);
  }

  Resource* GetResource(const std::string& url) const {
    if (resources.empty())
      return nullptr;

    const std::string& path_url = test_request::GetPathURL(url);
    ResourceList::const_iterator it = resources.begin();
    for (; it != resources.end(); ++it) {
      Resource* resource = *it;
      if (resource->GetPathURL() == path_url)
        return resource;
    }
    return nullptr;
  }

  Resource* GetResource(CefRefPtr<CefRequest> request) const {
    return GetResource(request->GetURL());
  }

  // Validate expected initial state.
  void Validate() const { DCHECK(!resources.empty()); }

  std::string GetMainURL() const { return resources.front()->GetPathURL(); }

  // Returns true if the server will be used.
  bool NeedsServer() const {
    ResourceList::const_iterator it = resources.begin();
    for (; it != resources.end(); ++it) {
      Resource* resource = *it;
      if (resource->handler == HandlerType::SERVER)
        return true;
    }
    return false;
  }

  // Returns true if all expectations have been met.
  bool IsDone() const {
    ResourceList::const_iterator it = resources.begin();
    for (; it != resources.end(); ++it) {
      Resource* resource = *it;
      if (!resource->IsDone())
        return false;
    }
    return true;
  }

  void AssertDone() const {
    ResourceList::const_iterator it = resources.begin();
    for (; it != resources.end(); ++it) {
      (*it)->AssertDone();
    }
  }
};

class TestServerObserver : public test_server::ObserverHelper {
 public:
  typedef base::Callback<bool()> CheckDoneCallback;

  TestServerObserver(TestSetup* setup,
                     const base::Closure& ready_callback,
                     const base::Closure& done_callback)
      : setup_(setup),
        ready_callback_(ready_callback),
        done_callback_(done_callback),
        weak_ptr_factory_(this) {
    DCHECK(setup);
    Initialize();
  }

  ~TestServerObserver() override { done_callback_.Run(); }

  void OnInitialized(const std::string& server_origin) override {
    CEF_REQUIRE_UI_THREAD();
    ready_callback_.Run();
  }

  bool OnHttpRequest(CefRefPtr<CefServer> server,
                     int connection_id,
                     const CefString& client_address,
                     CefRefPtr<CefRequest> request) override {
    CEF_REQUIRE_UI_THREAD();
    Resource* resource = setup_->GetResource(request);
    if (!resource) {
      // Not a request we handle.
      return false;
    }

    resource->response_ct++;
    EXPECT_TRUE(resource->VerifyRequest(request))
        << request->GetURL().ToString();
    test_server::SendResponse(server, connection_id, resource->response,
                              resource->response_data);

    // Stop propagating the callback.
    return true;
  }

  void OnShutdown() override {
    CEF_REQUIRE_UI_THREAD();
    delete this;
  }

 private:
  TestSetup* const setup_;
  const base::Closure ready_callback_;
  const base::Closure done_callback_;

  base::WeakPtrFactory<TestServerObserver> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(TestServerObserver);
};

class CorsTestHandler : public RoutingTestHandler {
 public:
  explicit CorsTestHandler(TestSetup* setup) : setup_(setup) {
    setup_->Validate();
  }

  void RunTest() override {
    StartServer(base::Bind(&CorsTestHandler::TriggerCreateBrowser, this));

    // Time out the test after a reasonable period of time.
    SetTestTimeout();
  }

  // Necessary to make the method public in order to destroy the test from
  // ClientSchemeHandlerType::ProcessRequest().
  void DestroyTest() override {
    EXPECT_TRUE(shutting_down_);

    setup_->AssertDone();
    EXPECT_TRUE(setup_->console_messages.empty())
        << "Did not receive expected console message: "
        << setup_->console_messages.front();

    RoutingTestHandler::DestroyTest();
  }

  void DestroyTestIfDone() {
    CEF_REQUIRE_UI_THREAD();
    if (shutting_down_)
      return;

    if (setup_->IsDone()) {
      shutting_down_ = true;
      StopServer();
    }
  }

  CefRefPtr<CefResourceHandler> GetResourceHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request) override {
    CEF_REQUIRE_IO_THREAD();
    Resource* resource = setup_->GetResource(request);
    if (resource && resource->handler != HandlerType::SERVER) {
      resource->response_ct++;
      EXPECT_TRUE(resource->VerifyRequest(request))
          << request->GetURL().ToString();
      return test_request::CreateResourceHandler(resource->response,
                                                 resource->response_data);
    }
    return RoutingTestHandler::GetResourceHandler(browser, frame, request);
  }

  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override {
    const std::string& url = frame->GetURL();
    Resource* resource = GetResource(url);
    if (!resource)
      return;

    const int expected_status = resource->response->GetStatus();
    if (url == main_url_ || expected_status != 200) {
      // Test that the status code is correct.
      EXPECT_EQ(expected_status, httpStatusCode) << url;
    }

    TriggerDestroyTestIfDone();
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode,
                   const CefString& errorText,
                   const CefString& failedUrl) override {
    Resource* resource = GetResource(failedUrl);
    if (!resource)
      return;

    const cef_errorcode_t expected_error = resource->response->GetError();

    // Tests sometimes also fail with ERR_ABORTED.
    if (!(expected_error == ERR_NONE && errorCode == ERR_ABORTED)) {
      EXPECT_EQ(expected_error, errorCode) << failedUrl.ToString();
    }

    TriggerDestroyTestIfDone();
  }

  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64 query_id,
               const CefString& request,
               bool persistent,
               CefRefPtr<Callback> callback) override {
    Resource* resource = GetResource(frame->GetURL());
    if (!resource)
      return false;

    if (request.ToString() == kSuccessMsg ||
        request.ToString() == kFailureMsg) {
      callback->Success("");
      if (request.ToString() == kSuccessMsg)
        resource->success_query_ct++;
      else
        resource->failure_query_ct++;
      TriggerDestroyTestIfDone();
      return true;
    }
    return false;
  }

  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                        cef_log_severity_t level,
                        const CefString& message,
                        const CefString& source,
                        int line) override {
    bool expected = false;
    if (!setup_->console_messages.empty()) {
      std::vector<std::string>::iterator it = setup_->console_messages.begin();
      for (; it != setup_->console_messages.end(); ++it) {
        const std::string& possible = *it;
        const std::string& actual = message.ToString();
        if (actual.find(possible) == 0U) {
          expected = true;
          setup_->console_messages.erase(it);
          break;
        }
      }
    }

    EXPECT_TRUE(expected) << "Unexpected console message: "
                          << message.ToString();
    return false;
  }

 protected:
  void TriggerCreateBrowser() {
    main_url_ = setup_->GetMainURL();
    CreateBrowser(main_url_);
  }

  void TriggerDestroyTestIfDone() {
    CefPostTask(TID_UI, base::Bind(&CorsTestHandler::DestroyTestIfDone, this));
  }

  void StartServer(const base::Closure& next_step) {
    if (!CefCurrentlyOn(TID_UI)) {
      CefPostTask(TID_UI,
                  base::Bind(&CorsTestHandler::StartServer, this, next_step));
      return;
    }

    if (!setup_->NeedsServer()) {
      next_step.Run();
      return;
    }

    // Will delete itself after the server stops.
    server_ = new TestServerObserver(
        setup_, next_step, base::Bind(&CorsTestHandler::StoppedServer, this));
  }

  void StopServer() {
    CEF_REQUIRE_UI_THREAD();
    if (!server_) {
      DCHECK(!setup_->NeedsServer());
      DestroyTest();
      return;
    }

    // Results in a call to StoppedServer().
    server_->Shutdown();
  }

  void StoppedServer() {
    CEF_REQUIRE_UI_THREAD();
    server_ = nullptr;
    DestroyTest();
  }

  Resource* GetResource(const std::string& url) const {
    Resource* resource = setup_->GetResource(url);
    EXPECT_TRUE(resource) << url;
    return resource;
  }

  TestSetup* setup_;
  std::string main_url_;
  TestServerObserver* server_ = nullptr;
  bool shutting_down_ = false;

  IMPLEMENT_REFCOUNTING(CorsTestHandler);
  DISALLOW_COPY_AND_ASSIGN(CorsTestHandler);
};

// JS that results in a call to CorsTestHandler::OnQuery.
std::string GetMsgJS(const std::string& msg) {
  return "window.testQuery({request:'" + msg + "'});";
}

std::string GetSuccessMsgJS() {
  return GetMsgJS(kSuccessMsg);
}
std::string GetFailureMsgJS() {
  return GetMsgJS(kFailureMsg);
}

std::string GetDefaultSuccessMsgHtml() {
  return "<html><body>TEST<script>" + GetSuccessMsgJS() +
         "</script></body></html>";
}

}  // namespace

// Verify the test harness for server requests.
TEST(CorsTest, BasicServer) {
  TestSetup setup;
  Resource resource(HandlerType::SERVER, "/CorsTest.BasicServer");
  setup.AddResource(&resource);

  CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);
}

// Like above, but also send a query JS message.
TEST(CorsTest, BasicServerWithQuery) {
  TestSetup setup;
  Resource resource(HandlerType::SERVER, "/CorsTest.BasicServerWithQuery",
                    kMimeTypeHtml, GetDefaultSuccessMsgHtml());
  resource.expected_success_query_ct = 1;
  setup.AddResource(&resource);

  CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);
}

// Verify the test harness for http scheme requests.
TEST(CorsTest, BasicHttpScheme) {
  TestSetup setup;
  Resource resource(HandlerType::HTTP_SCHEME, "/CorsTest.BasicHttpScheme");
  setup.AddResource(&resource);

  CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);
}

// Like above, but also send a query JS message.
TEST(CorsTest, BasicHttpSchemeWithQuery) {
  TestSetup setup;
  Resource resource(HandlerType::HTTP_SCHEME,
                    "/CorsTest.BasicHttpSchemeWithQuery", kMimeTypeHtml,
                    GetDefaultSuccessMsgHtml());
  resource.expected_success_query_ct = 1;
  setup.AddResource(&resource);

  CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);
}

// Verify the test harness for custom standard scheme requests.
TEST(CorsTest, BasicCustomStandardScheme) {
  TestSetup setup;
  Resource resource(HandlerType::CUSTOM_STANDARD_SCHEME,
                    "/CorsTest.BasicCustomStandardScheme");
  setup.AddResource(&resource);

  CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);
}

// Like above, but also send a query JS message.
TEST(CorsTest, BasicCustomStandardSchemeWithQuery) {
  TestSetup setup;
  Resource resource(HandlerType::CUSTOM_STANDARD_SCHEME,
                    "/CorsTest.BasicCustomStandardSchemeWithQuery",
                    kMimeTypeHtml, GetDefaultSuccessMsgHtml());
  resource.expected_success_query_ct = 1;
  setup.AddResource(&resource);

  CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);
  handler->ExecuteTest();
  ReleaseAndWaitForDestructor(handler);
}

namespace {

std::string GetIframeMainHtml(const std::string& iframe_url,
                              const std::string& sandbox_attribs) {
  return "<html><body>TEST<iframe src=\"" + iframe_url + "\" sandbox=\"" +
         sandbox_attribs + "\"></iframe></body></html>";
}

std::string GetIframeSubHtml() {
  // Try to script the parent frame, then send the SuccessMsg.
  return "<html><body>TEST<script>try { parent.document.body; } catch "
         "(exception) { console.log(exception.toString()); }" +
         GetSuccessMsgJS() + "</script></body></html>";
}

bool HasSandboxAttrib(const std::string& sandbox_attribs,
                      const std::string& attrib) {
  return sandbox_attribs.find(attrib) != std::string::npos;
}

void SetupIframeRequest(TestSetup* setup,
                        const std::string& test_name,
                        HandlerType main_handler,
                        Resource* main_resource,
                        HandlerType iframe_handler,
                        Resource* iframe_resource,
                        const std::string& sandbox_attribs) {
  const std::string& base_path = "/" + test_name;

  // Expect a single iframe request.
  iframe_resource->Init(iframe_handler, base_path + ".iframe.html",
                        kMimeTypeHtml, GetIframeSubHtml());

  // Expect a single main frame request.
  const std::string& iframe_url = iframe_resource->GetPathURL();
  main_resource->Init(main_handler, base_path, kMimeTypeHtml,
                      GetIframeMainHtml(iframe_url, sandbox_attribs));

  if (HasSandboxAttrib(sandbox_attribs, "allow-scripts")) {
    // Expect the iframe to load successfully and send the SuccessMsg.
    iframe_resource->expected_success_query_ct = 1;

    const bool has_same_origin =
        HasSandboxAttrib(sandbox_attribs, "allow-same-origin");
    if (!has_same_origin ||
        (has_same_origin &&
         (main_handler == HandlerType::CUSTOM_NONSTANDARD_SCHEME ||
          main_handler != iframe_handler))) {
      // Expect parent frame scripting to fail if:
      // - "allow-same-origin" is not specified;
      // - the main frame is a non-standard scheme (e.g. CORS disabled);
      // - the main frame and iframe origins don't match.
      // The reported origin will be "null" if "allow-same-origin" is not
      // specified, or if the iframe is hosted on a non-standard scheme.
      const std::string& origin =
          !has_same_origin ||
                  iframe_handler == HandlerType::CUSTOM_NONSTANDARD_SCHEME
              ? "null"
              : GetOrigin(iframe_handler);
      setup->AddConsoleMessage("SecurityError: Blocked a frame with origin \"" +
                               origin +
                               "\" from accessing a cross-origin frame.");
    }
  } else {
    // Expect JavaScript execution to fail.
    setup->AddConsoleMessage("Blocked script execution in '" + iframe_url +
                             "' because the document's frame is sandboxed and "
                             "the 'allow-scripts' permission is not set.");
  }

  setup->AddResource(main_resource);
  setup->AddResource(iframe_resource);
}

}  // namespace

// Test iframe sandbox attributes with different origin combinations.
#define CORS_TEST_IFRAME(test_name, handler_main, handler_iframe,     \
                         sandbox_attribs)                             \
  TEST(CorsTest, Iframe##test_name) {                                 \
    TestSetup setup;                                                  \
    Resource resource_main, resource_iframe;                          \
    SetupIframeRequest(&setup, "CorsTest.Iframe" #test_name,          \
                       HandlerType::handler_main, &resource_main,     \
                       HandlerType::handler_iframe, &resource_iframe, \
                       sandbox_attribs);                              \
    CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup); \
    handler->ExecuteTest();                                           \
    ReleaseAndWaitForDestructor(handler);                             \
  }

// Test all origin combinations (same and cross-origin).
#define CORS_TEST_IFRAME_ALL(name, sandbox_attribs)                            \
  CORS_TEST_IFRAME(name##ServerToServer, SERVER, SERVER, sandbox_attribs)      \
  CORS_TEST_IFRAME(name##ServerToHttpScheme, SERVER, HTTP_SCHEME,              \
                   sandbox_attribs)                                            \
  CORS_TEST_IFRAME(name##ServerToCustomStandardScheme, SERVER,                 \
                   CUSTOM_STANDARD_SCHEME, sandbox_attribs)                    \
  CORS_TEST_IFRAME(name##ServerToCustomNonStandardScheme, SERVER,              \
                   CUSTOM_NONSTANDARD_SCHEME, sandbox_attribs)                 \
  CORS_TEST_IFRAME(name##HttpSchemeToServer, HTTP_SCHEME, SERVER,              \
                   sandbox_attribs)                                            \
  CORS_TEST_IFRAME(name##HttpSchemeToHttpScheme, HTTP_SCHEME, HTTP_SCHEME,     \
                   sandbox_attribs)                                            \
  CORS_TEST_IFRAME(name##HttpSchemeToCustomStandardScheme, HTTP_SCHEME,        \
                   CUSTOM_STANDARD_SCHEME, sandbox_attribs)                    \
  CORS_TEST_IFRAME(name##HttpSchemeToCustomNonStandardScheme, HTTP_SCHEME,     \
                   CUSTOM_NONSTANDARD_SCHEME, sandbox_attribs)                 \
  CORS_TEST_IFRAME(name##CustomStandardSchemeToServer, CUSTOM_STANDARD_SCHEME, \
                   SERVER, sandbox_attribs)                                    \
  CORS_TEST_IFRAME(name##CustomStandardSchemeToHttpScheme,                     \
                   CUSTOM_STANDARD_SCHEME, HTTP_SCHEME, sandbox_attribs)       \
  CORS_TEST_IFRAME(name##CustomStandardSchemeToCustomStandardScheme,           \
                   CUSTOM_STANDARD_SCHEME, CUSTOM_STANDARD_SCHEME,             \
                   sandbox_attribs)                                            \
  CORS_TEST_IFRAME(name##CustomStandardSchemeToCustomNonStandardScheme,        \
                   CUSTOM_STANDARD_SCHEME, CUSTOM_NONSTANDARD_SCHEME,          \
                   sandbox_attribs)                                            \
  CORS_TEST_IFRAME(name##CustomNonStandardSchemeToServer,                      \
                   CUSTOM_NONSTANDARD_SCHEME, SERVER, sandbox_attribs)         \
  CORS_TEST_IFRAME(name##CustomNonStandardSchemeToHttpScheme,                  \
                   CUSTOM_NONSTANDARD_SCHEME, HTTP_SCHEME, sandbox_attribs)    \
  CORS_TEST_IFRAME(name##CustomNonStandardSchemeToCustomStandardScheme,        \
                   CUSTOM_NONSTANDARD_SCHEME, CUSTOM_STANDARD_SCHEME,          \
                   sandbox_attribs)                                            \
  CORS_TEST_IFRAME(name##CustomNonStandardSchemeToCustomNonStandardScheme,     \
                   CUSTOM_NONSTANDARD_SCHEME, CUSTOM_NONSTANDARD_SCHEME,       \
                   sandbox_attribs)

// Everything is blocked.
CORS_TEST_IFRAME_ALL(None, "")

// JavaScript execution is allowed.
CORS_TEST_IFRAME_ALL(AllowScripts, "allow-scripts")

// JavaScript execution is allowed and scripting the parent is allowed for
// same-origin only.
CORS_TEST_IFRAME_ALL(AllowScriptsAndSameOrigin,
                     "allow-scripts allow-same-origin")

namespace {

struct SubResource : Resource {
  SubResource() {}

  std::string main_origin;
  bool supports_cors = false;
  bool is_cross_origin = false;

  void InitCors(HandlerType main_handler, bool add_header) {
    // Origin is always "null" for non-standard schemes.
    main_origin = main_handler == HandlerType::CUSTOM_NONSTANDARD_SCHEME
                      ? "null"
                      : GetOrigin(main_handler);

    // True if cross-origin requests are allowed. XHR requests to non-standard
    // schemes are not allowed (due to the "null" origin).
    supports_cors = handler != HandlerType::CUSTOM_NONSTANDARD_SCHEME;
    if (!supports_cors) {
      // Don't expect the xhr request.
      expected_response_ct = 0;
    }

    // True if the request is considered cross-origin. Any requests between
    // non-standard schemes are considered cross-origin (due to the "null"
    // origin).
    is_cross_origin = main_handler != handler ||
                      (main_handler == HandlerType::CUSTOM_NONSTANDARD_SCHEME &&
                       handler == main_handler);

    if (is_cross_origin && add_header) {
      response->SetHeaderByName("Access-Control-Allow-Origin", main_origin,
                                false);
    }
  }

  bool VerifyRequest(CefRefPtr<CefRequest> request) const override {
    // Verify that the "Origin" header contains the expected value.
    const std::string& origin = request->GetHeaderByName("Origin");
    if (is_cross_origin) {
      EXPECT_STREQ(main_origin.c_str(), origin.c_str());
      return main_origin == origin;
    }
    EXPECT_TRUE(origin.empty());
    return origin.empty();
  }
};

enum class ExecMode {
  XHR,
  FETCH,
};

std::string GetXhrExecJS(const std::string& sub_url) {
  return "xhr = new XMLHttpRequest();\n"
         "xhr.open(\"GET\", \"" +
         sub_url +
         "\", true)\n;"
         "xhr.onload = function(e) {\n"
         "  if (xhr.readyState === 4) {\n"
         "    if (xhr.status === 200) {\n"
         "      onResult(xhr.responseText);\n"
         "    } else {\n"
         "      console.log('XMLHttpRequest failed with status ' + "
         "xhr.status);\n"
         "      onResult('FAILURE');\n"
         "    }\n"
         "  }\n"
         "};\n"
         "xhr.onerror = function(e) {\n"
         "  onResult('FAILURE');\n"
         "};\n"
         "xhr.send();\n";
}

std::string GetFetchExecJS(const std::string& sub_url) {
  return "fetch('" + sub_url +
         "')\n"
         ".then(function(response) {\n"
         "  if (response.status === 200) {\n"
         "      response.text().then(function(text) {\n"
         "          onResult(text);\n"
         "      }).catch(function(e) {\n"
         "          onResult('FAILURE')\n;        "
         "      })\n;"
         "  } else {\n"
         "      onResult('FAILURE');\n"
         "  }\n"
         "}).catch(function(e) {\n"
         "  onResult('FAILURE');\n"
         "});\n";
}

std::string GetExecMainHtml(ExecMode mode, const std::string& sub_url) {
  return std::string() +
         "<html><head>\n"
         "<script language=\"JavaScript\">\n" +
         "function onResult(val) {\n"
         "  if (val === '" +
         kDefaultText + "') {" + GetSuccessMsgJS() + "} else {" +
         GetFailureMsgJS() +
         "}\n}\n"
         "function execRequest() {\n" +
         (mode == ExecMode::XHR ? GetXhrExecJS(sub_url)
                                : GetFetchExecJS(sub_url)) +
         "}\n</script>\n"
         "</head><body onload=\"execRequest();\">"
         "Running execRequest..."
         "</body></html>";
}

// XHR and fetch requests behave the same, except for console message contents.
void SetupExecRequest(ExecMode mode,
                      TestSetup* setup,
                      const std::string& test_name,
                      HandlerType main_handler,
                      Resource* main_resource,
                      HandlerType sub_handler,
                      SubResource* sub_resource,
                      bool add_header) {
  const std::string& base_path = "/" + test_name;

  // Expect a single xhr request.
  sub_resource->Init(sub_handler, base_path + ".sub.txt", kMimeTypeText,
                     kDefaultText);
  sub_resource->InitCors(main_handler, add_header);

  // Expect a single main frame request.
  const std::string& sub_url = sub_resource->GetPathURL();
  main_resource->Init(main_handler, base_path, kMimeTypeHtml,
                      GetExecMainHtml(mode, sub_url));

  if (sub_resource->is_cross_origin &&
      (!sub_resource->supports_cors || !add_header)) {
    // Expect the cross-origin XHR to be blocked.
    main_resource->expected_failure_query_ct = 1;

    if (sub_resource->supports_cors && !add_header) {
      // The request supports CORS, but we didn't add the header.
      if (mode == ExecMode::XHR) {
        setup->AddConsoleMessage(
            "Access to XMLHttpRequest at '" + sub_url + "' from origin '" +
            sub_resource->main_origin +
            "' has been blocked by CORS policy: No "
            "'Access-Control-Allow-Origin' "
            "header is present on the requested resource.");
      } else {
        setup->AddConsoleMessage(
            "Access to fetch at '" + sub_url + "' from origin '" +
            sub_resource->main_origin +
            "' has been blocked by CORS policy: No "
            "'Access-Control-Allow-Origin' header is present on the requested "
            "resource. If an opaque response serves your needs, set the "
            "request's mode to 'no-cors' to fetch the resource with CORS "
            "disabled.");
      }
    } else if (mode == ExecMode::XHR) {
      setup->AddConsoleMessage(
          "Access to XMLHttpRequest at '" + sub_url + "' from origin '" +
          sub_resource->main_origin +
          "' has been blocked by CORS policy: Cross origin requests are only "
          "supported for protocol schemes:");
    } else {
      setup->AddConsoleMessage(
          "Fetch API cannot load " + sub_url +
          ". URL scheme must be \"http\" or \"https\" for CORS request.");
    }
  } else {
    // Expect the (possibly cross-origin) XHR to be allowed.
    main_resource->expected_success_query_ct = 1;
  }

  setup->AddResource(main_resource);
  setup->AddResource(sub_resource);
}

}  // namespace

// Test XHR requests with different origin combinations.
#define CORS_TEST_XHR(test_name, handler_main, handler_sub, add_header)    \
  TEST(CorsTest, Xhr##test_name) {                                         \
    TestSetup setup;                                                       \
    Resource resource_main;                                                \
    SubResource resource_sub;                                              \
    SetupExecRequest(ExecMode::XHR, &setup, "CorsTest.Xhr" #test_name,     \
                     HandlerType::handler_main, &resource_main,            \
                     HandlerType::handler_sub, &resource_sub, add_header); \
    CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);      \
    handler->ExecuteTest();                                                \
    ReleaseAndWaitForDestructor(handler);                                  \
  }

// Test all origin combinations (same and cross-origin).
#define CORS_TEST_XHR_ALL(name, add_header)                                    \
  CORS_TEST_XHR(name##ServerToServer, SERVER, SERVER, add_header)              \
  CORS_TEST_XHR(name##ServerToHttpScheme, SERVER, HTTP_SCHEME, add_header)     \
  CORS_TEST_XHR(name##ServerToCustomStandardScheme, SERVER,                    \
                CUSTOM_STANDARD_SCHEME, add_header)                            \
  CORS_TEST_XHR(name##ServerToCustomNonStandardScheme, SERVER,                 \
                CUSTOM_NONSTANDARD_SCHEME, add_header)                         \
  CORS_TEST_XHR(name##HttpSchemeToServer, HTTP_SCHEME, SERVER, add_header)     \
  CORS_TEST_XHR(name##HttpSchemeToHttpScheme, HTTP_SCHEME, HTTP_SCHEME,        \
                add_header)                                                    \
  CORS_TEST_XHR(name##HttpSchemeToCustomStandardScheme, HTTP_SCHEME,           \
                CUSTOM_STANDARD_SCHEME, add_header)                            \
  CORS_TEST_XHR(name##HttpSchemeToCustomNonStandardScheme, HTTP_SCHEME,        \
                CUSTOM_NONSTANDARD_SCHEME, add_header)                         \
  CORS_TEST_XHR(name##CustomStandardSchemeToServer, CUSTOM_STANDARD_SCHEME,    \
                SERVER, add_header)                                            \
  CORS_TEST_XHR(name##CustomStandardSchemeToHttpScheme,                        \
                CUSTOM_STANDARD_SCHEME, HTTP_SCHEME, add_header)               \
  CORS_TEST_XHR(name##CustomStandardSchemeToCustomStandardScheme,              \
                CUSTOM_STANDARD_SCHEME, CUSTOM_STANDARD_SCHEME, add_header)    \
  CORS_TEST_XHR(name##CustomStandardSchemeToCustomNonStandardScheme,           \
                CUSTOM_STANDARD_SCHEME, CUSTOM_NONSTANDARD_SCHEME, add_header) \
  CORS_TEST_XHR(name##CustomNonStandardSchemeToServer,                         \
                CUSTOM_NONSTANDARD_SCHEME, SERVER, add_header)                 \
  CORS_TEST_XHR(name##CustomNonStandardSchemeToHttpScheme,                     \
                CUSTOM_NONSTANDARD_SCHEME, HTTP_SCHEME, add_header)            \
  CORS_TEST_XHR(name##CustomNonStandardSchemeToCustomStandardScheme,           \
                CUSTOM_NONSTANDARD_SCHEME, CUSTOM_STANDARD_SCHEME, add_header) \
  CORS_TEST_XHR(name##CustomNonStandardSchemeToCustomNonStandardScheme,        \
                CUSTOM_NONSTANDARD_SCHEME, CUSTOM_NONSTANDARD_SCHEME,          \
                add_header)

// XHR requests without the "Access-Control-Allow-Origin" header.
CORS_TEST_XHR_ALL(NoHeader, false)

// XHR requests with the "Access-Control-Allow-Origin" header.
CORS_TEST_XHR_ALL(WithHeader, true)

// Test fetch requests with different origin combinations.
#define CORS_TEST_FETCH(test_name, handler_main, handler_sub, add_header)  \
  TEST(CorsTest, Fetch##test_name) {                                       \
    TestSetup setup;                                                       \
    Resource resource_main;                                                \
    SubResource resource_sub;                                              \
    SetupExecRequest(ExecMode::FETCH, &setup, "CorsTest.Fetch" #test_name, \
                     HandlerType::handler_main, &resource_main,            \
                     HandlerType::handler_sub, &resource_sub, add_header); \
    CefRefPtr<CorsTestHandler> handler = new CorsTestHandler(&setup);      \
    handler->ExecuteTest();                                                \
    ReleaseAndWaitForDestructor(handler);                                  \
  }

// Test all origin combinations (same and cross-origin).
#define CORS_TEST_FETCH_ALL(name, add_header)                                 \
  CORS_TEST_FETCH(name##ServerToServer, SERVER, SERVER, add_header)           \
  CORS_TEST_FETCH(name##ServerToHttpScheme, SERVER, HTTP_SCHEME, add_header)  \
  CORS_TEST_FETCH(name##ServerToCustomStandardScheme, SERVER,                 \
                  CUSTOM_STANDARD_SCHEME, add_header)                         \
  CORS_TEST_FETCH(name##ServerToCustomNonStandardScheme, SERVER,              \
                  CUSTOM_NONSTANDARD_SCHEME, add_header)                      \
  CORS_TEST_FETCH(name##HttpSchemeToServer, HTTP_SCHEME, SERVER, add_header)  \
  CORS_TEST_FETCH(name##HttpSchemeToHttpScheme, HTTP_SCHEME, HTTP_SCHEME,     \
                  add_header)                                                 \
  CORS_TEST_FETCH(name##HttpSchemeToCustomStandardScheme, HTTP_SCHEME,        \
                  CUSTOM_STANDARD_SCHEME, add_header)                         \
  CORS_TEST_FETCH(name##HttpSchemeToCustomNonStandardScheme, HTTP_SCHEME,     \
                  CUSTOM_NONSTANDARD_SCHEME, add_header)                      \
  CORS_TEST_FETCH(name##CustomStandardSchemeToServer, CUSTOM_STANDARD_SCHEME, \
                  SERVER, add_header)                                         \
  CORS_TEST_FETCH(name##CustomStandardSchemeToHttpScheme,                     \
                  CUSTOM_STANDARD_SCHEME, HTTP_SCHEME, add_header)            \
  CORS_TEST_FETCH(name##CustomStandardSchemeToCustomStandardScheme,           \
                  CUSTOM_STANDARD_SCHEME, CUSTOM_STANDARD_SCHEME, add_header) \
  CORS_TEST_FETCH(name##CustomStandardSchemeToCustomNonStandardScheme,        \
                  CUSTOM_STANDARD_SCHEME, CUSTOM_NONSTANDARD_SCHEME,          \
                  add_header)                                                 \
  CORS_TEST_FETCH(name##CustomNonStandardSchemeToServer,                      \
                  CUSTOM_NONSTANDARD_SCHEME, SERVER, add_header)              \
  CORS_TEST_FETCH(name##CustomNonStandardSchemeToHttpScheme,                  \
                  CUSTOM_NONSTANDARD_SCHEME, HTTP_SCHEME, add_header)         \
  CORS_TEST_FETCH(name##CustomNonStandardSchemeToCustomStandardScheme,        \
                  CUSTOM_NONSTANDARD_SCHEME, CUSTOM_STANDARD_SCHEME,          \
                  add_header)                                                 \
  CORS_TEST_FETCH(name##CustomNonStandardSchemeToCustomNonStandardScheme,     \
                  CUSTOM_NONSTANDARD_SCHEME, CUSTOM_NONSTANDARD_SCHEME,       \
                  add_header)

// Fetch requests without the "Access-Control-Allow-Origin" header.
CORS_TEST_FETCH_ALL(NoHeader, false)

// Fetch requests with the "Access-Control-Allow-Origin" header.
CORS_TEST_FETCH_ALL(WithHeader, true)
