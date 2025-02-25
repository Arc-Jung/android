// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_script_loader_factory.h"

#include <memory>
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_installed_script_loader.h"
#include "content/browser/service_worker/service_worker_new_script_loader.h"
#include "content/browser/service_worker/service_worker_provider_host.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/common/service_worker/service_worker_utils.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "services/network/public/cpp/resource_response.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace content {

ServiceWorkerScriptLoaderFactory::ServiceWorkerScriptLoaderFactory(
    base::WeakPtr<ServiceWorkerContextCore> context,
    base::WeakPtr<ServiceWorkerProviderHost> provider_host,
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory)
    : context_(context),
      provider_host_(provider_host),
      loader_factory_(std::move(loader_factory)) {
  DCHECK(provider_host_->IsProviderForServiceWorker());
  DCHECK(loader_factory_);
}

ServiceWorkerScriptLoaderFactory::~ServiceWorkerScriptLoaderFactory() = default;

void ServiceWorkerScriptLoaderFactory::CreateLoaderAndStart(
    network::mojom::URLLoaderRequest request,
    int32_t routing_id,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& resource_request,
    network::mojom::URLLoaderClientPtr client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  DCHECK(ServiceWorkerUtils::IsServicificationEnabled());
  if (!CheckIfScriptRequestIsValid(resource_request)) {
    // If the request should not be handled, just do a passthrough load. This
    // needs a relaying as we use different associated message pipes.
    // TODO(kinuko): Record the reason like what we do with netlog in
    // ServiceWorkerContextRequestHandler.
    loader_factory_->CreateLoaderAndStart(
        std::move(request), routing_id, request_id, options, resource_request,
        std::move(client), traffic_annotation);
    return;
  }

  // There are four cases of how to handle the request for the script.
  // A) service worker is installed, script is installed: serve from storage
  //    (use ServceWorkerInstalledScriptLoader). Typically this case is handled
  //    by ServiceWorkerInstalledScriptsSender, but we can still get here when a
  //    new service worker starts up and becomes installed while it is running.
  // B) service worker is installed, script is not installed: serve from direct
  //    network. This happens when the script is newly imported after
  //    installation.
  //    TODO(crbug.com/719052): deprecate this.
  // C) service worker is not installed, script is installed: serve from
  //    storage (use ServceWorkerInstalledScriptLoader)
  // D) service worker is not installed, script is not installed: serve from
  //    network with installing the script (use ServceWorkerNewScriptLoader)
  //    This is the common case: load the script and install it.

  // Case A and C:
  scoped_refptr<ServiceWorkerVersion> version =
      provider_host_->running_hosted_version();
  int64_t resource_id =
      version->script_cache_map()->LookupResourceId(resource_request.url);
  if (resource_id != kInvalidServiceWorkerResourceId) {
    std::unique_ptr<ServiceWorkerResponseReader> response_reader =
        context_->storage()->CreateResponseReader(resource_id);
    mojo::MakeStrongBinding(
        std::make_unique<ServiceWorkerInstalledScriptLoader>(
            options, std::move(client), std::move(response_reader)),
        std::move(request));
    return;
  }

  // Case B:
  if (ServiceWorkerVersion::IsInstalled(version->status())) {
    // TODO(kinuko): Record the reason like what we do with netlog in
    // ServiceWorkerContextRequestHandler.
    loader_factory_->CreateLoaderAndStart(
        std::move(request), routing_id, request_id, options, resource_request,
        std::move(client), traffic_annotation);
    return;
  }

  // Case D:
  mojo::MakeStrongBinding(
      std::make_unique<ServiceWorkerNewScriptLoader>(
          routing_id, request_id, options, resource_request, std::move(client),
          provider_host_->running_hosted_version(), loader_factory_,
          traffic_annotation),
      std::move(request));
}

void ServiceWorkerScriptLoaderFactory::Clone(
    network::mojom::URLLoaderFactoryRequest request) {
  bindings_.AddBinding(this, std::move(request));
}

bool ServiceWorkerScriptLoaderFactory::CheckIfScriptRequestIsValid(
    const network::ResourceRequest& resource_request) {
  if (!context_ || !provider_host_)
    return false;

  scoped_refptr<ServiceWorkerVersion> version =
      provider_host_->running_hosted_version();
  if (!version)
    return false;

  // Handle only the service worker main script (RESOURCE_TYPE_SERVICE_WORKER)
  // or importScripts() (RESOURCE_TYPE_SCRIPT).
  switch (resource_request.resource_type) {
    case RESOURCE_TYPE_SERVICE_WORKER:
      // The main script should be fetched only when we start a new service
      // worker.
      if (version->status() != ServiceWorkerVersion::NEW)
        return false;
      break;
    case RESOURCE_TYPE_SCRIPT:
      // TODO(nhiroki): In the current implementation, importScripts() can be
      // called in any ServiceWorkerVersion::Status except for REDUNDANT, but
      // the spec defines importScripts() works only on the initial script
      // evaluation and the install event. Update this check once
      // importScripts() is fixed (https://crbug.com/719052).
      if (version->status() == ServiceWorkerVersion::REDUNDANT) {
        // This could happen if browser-side has set the status to redundant but
        // the worker has not yet stopped. The worker is already doomed so just
        // reject the request. Handle it specially here because otherwise it'd
        // be unclear whether "REDUNDANT" should count as installed or not
        // installed when making decisions about how to handle the request and
        // logging UMA.
        return false;
      }
      break;
    default:
      // TODO(nhiroki): Record bad message, we shouldn't come here for other
      // request types.
      NOTREACHED();
      return false;
  }

  // TODO(falken): Make sure we don't handle a redirected request.

  return true;
}

}  // namespace content
