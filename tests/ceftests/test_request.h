// Copyright (c) 2020 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFTESTS_TEST_REQUEST_H_
#define CEF_TESTS_CEFTESTS_TEST_REQUEST_H_
#pragma once

#include <string>

#include "include/base/cef_bind.h"
#include "include/cef_frame.h"
#include "include/cef_request.h"
#include "include/cef_request_context.h"
#include "include/cef_resource_handler.h"
#include "include/cef_response.h"

namespace test_request {

// Stores all state passed to CefURLRequestClient.
struct State {
 public:
  // Number of times each callback is executed.
  int upload_progress_ct_ = 0;
  int download_progress_ct_ = 0;
  int download_data_ct_ = 0;
  int auth_credentials_ct_ = 0;
  int request_complete_ct_ = 0;

  // From OnUploadProgress.
  int64 upload_total_ = 0;

  // From OnDownloadProgress.
  int64 download_total_ = 0;

  // From OnDownloadData.
  std::string download_data_;

  // From OnRequestComplete.
  CefRefPtr<CefRequest> request_;
  cef_urlrequest_status_t status_ = UR_UNKNOWN;
  cef_errorcode_t error_code_ = ERR_NONE;
  CefRefPtr<CefResponse> response_;
  bool response_was_cached_ = false;
};

typedef base::Callback<void(const State& state)> RequestDoneCallback;

struct SendConfig {
  // Send using |frame_| or |request_context_| if non-nullptr.
  // Sends using the global request context if both are nullptr.
  CefRefPtr<CefFrame> frame_;
  CefRefPtr<CefRequestContext> request_context_;

  // The request to send.
  CefRefPtr<CefRequest> request_;

  // Returned via GetAuthCredentials if |has_credentials_| is true.
  bool has_credentials_ = false;
  std::string username_;
  std::string password_;
};

// Send a request. |callback| will be executed on the calling thread after the
// request completes.
void Send(const SendConfig& config, const RequestDoneCallback& callback);

// Removes query and/or fragment components from |url|.
std::string GetPathURL(const std::string& url);

// Creates a new resource handler that returns the specified response.
CefRefPtr<CefResourceHandler> CreateResourceHandler(
    CefRefPtr<CefResponse> response,
    const std::string& response_data);

}  // namespace test_request

#endif  // CEF_TESTS_CEFTESTS_TEST_REQUEST_H_
