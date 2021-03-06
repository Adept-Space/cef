// Copyright (c) 2020 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_LIBCEF_BROWSER_MEDIA_ROUTER_MEDIA_SOURCE_IMPL_H_
#define CEF_LIBCEF_BROWSER_MEDIA_ROUTER_MEDIA_SOURCE_IMPL_H_
#pragma once

#include "include/cef_media_router.h"

#include "chrome/common/media_router/media_source.h"

// Implementation of the CefMediaSource interface. May be created on any thread.
class CefMediaSourceImpl : public CefMediaSource {
 public:
  explicit CefMediaSourceImpl(const media_router::MediaSource::Id& source_id);
  explicit CefMediaSourceImpl(const GURL& presentation_url);

  // CefMediaSource methods.
  CefString GetId() override;
  bool IsCastSource() override;
  bool IsDialSource() override;

  const media_router::MediaSource& source() const { return source_; }

 private:
  // Read-only after creation.
  const media_router::MediaSource source_;

  IMPLEMENT_REFCOUNTING(CefMediaSourceImpl);
  DISALLOW_COPY_AND_ASSIGN(CefMediaSourceImpl);
};

#endif  // CEF_LIBCEF_BROWSER_MEDIA_ROUTER_MEDIA_SOURCE_IMPL_H_
