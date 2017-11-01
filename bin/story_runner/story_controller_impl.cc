// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/story_runner/story_controller_impl.h"

#include <memory>

#include "lib/app/cpp/application_context.h"
#include "lib/app/cpp/connect.h"
#include "lib/app/fidl/application_launcher.fidl.h"
#include "lib/app/fidl/service_provider.fidl.h"
#include "lib/fidl/cpp/bindings/interface_handle.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/fidl/cpp/bindings/struct_ptr.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/join_strings.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "lib/module/fidl/module_context.fidl.h"
#include "lib/module/fidl/module_data.fidl.h"
#include "lib/story/fidl/link.fidl.h"
#include "lib/story/fidl/story_marker.fidl.h"
#include "lib/ui/views/fidl/view_provider.fidl.h"
#include "peridot/bin/story_runner/link_impl.h"
#include "peridot/bin/story_runner/module_context_impl.h"
#include "peridot/bin/story_runner/module_controller_impl.h"
#include "peridot/bin/story_runner/story_provider_impl.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/ledger_client/operations.h"
#include "peridot/lib/ledger_client/storage.h"

namespace modular {

constexpr char kStoryScopeLabelPrefix[] = "story-";

namespace {

fidl::String PathString(const fidl::Array<fidl::String>& module_path) {
  return fxl::JoinStrings(module_path, ":");
}

void XdrLinkPath(XdrContext* const xdr, LinkPath* const data) {
  xdr->Field("module_path", &data->module_path);
  xdr->Field("link_name", &data->link_name);
}

void XdrSurfaceRelation(XdrContext* const xdr, SurfaceRelation* const data) {
  xdr->Field("arrangement", &data->arrangement);
  xdr->Field("dependency", &data->dependency);
  xdr->Field("emphasis", &data->emphasis);
}

void XdrModuleData(XdrContext* const xdr, ModuleData* const data) {
  xdr->Field("url", &data->module_url);
  xdr->Field("module_path", &data->module_path);
  // TODO(mesch): Rename the XDR field eventually.
  xdr->Field("default_link_path", &data->link_path, XdrLinkPath);
  xdr->Field("module_source", &data->module_source);
  xdr->Field("surface_relation", &data->surface_relation, XdrSurfaceRelation);
  xdr->Field("module_stopped", &data->module_stopped);
}

void XdrPerDeviceStoryInfo(XdrContext* const xdr,
                           PerDeviceStoryInfo* const info) {
  xdr->Field("device", &info->device_id);
  xdr->Field("id", &info->story_id);
  xdr->Field("time", &info->timestamp);
  xdr->Field("state", &info->state);
}

void XdrStoryContextLog(XdrContext* const xdr, StoryContextLog* const data) {
  xdr->Field("context", &data->context);
  xdr->Field("device_id", &data->device_id);
  xdr->Field("time", &data->time);
  xdr->Field("signal", &data->signal);
}

}  // namespace

// A base class meant to be used by operations that want to get blocked until
// an asynchronous event happens outside that operation. The operation would
// call Block(callback) and yield control back to the message loop. Later an
// asynchronous event can call operation->Continue() that would run the
// callback.
// NOTE(alhaad): FlowTokens are NOT meant to be used with Blockable operations.
// This is because we'd need to store FlowToken instance in a class member. If
// we don't delete the class member, the refcount remains non-zero which cause
// an operation to stall forever. Deleting the flow token causes the operation
// itself to be deleted and needs to be done very carefully.
// TODO(alhaad): Consider using FlowTokenHolder.
class StoryControllerImpl::Blockable {
 public:
  Blockable(StoryControllerImpl* const story_controller_impl)
      : story_controller_impl_(story_controller_impl) {}

  void Continue() {
    continue_called_ = true;
    if (continue_) {
      continue_();
    }
  }

 protected:
  void Block(ModuleDataPtr module_data, std::function<void()> c) {
    story_controller_impl_->blocked_operations_.push_back(
        std::make_pair<ModuleDataPtr, Blockable*>(std::move(module_data),
                                                  this));
    continue_ = c;
    if (continue_called_) {
      continue_();
    }
  }

 private:
  StoryControllerImpl* const story_controller_impl_;  // not owned

  std::function<void()> continue_;
  bool continue_called_{};

  FXL_DISALLOW_COPY_AND_ASSIGN(Blockable);
};

class StoryControllerImpl::StoryMarkerImpl : StoryMarker {
 public:
  StoryMarkerImpl() = default;
  ~StoryMarkerImpl() override = default;

  void Connect(fidl::InterfaceRequest<StoryMarker> request) {
    bindings_.AddBinding(this, std::move(request));
  }

 private:
  fidl::BindingSet<StoryMarker> bindings_;
  FXL_DISALLOW_COPY_AND_ASSIGN(StoryMarkerImpl);
};

class StoryControllerImpl::LaunchModuleCall : Operation<> {
 public:
  LaunchModuleCall(
      OperationContainer* const container,
      StoryControllerImpl* const story_controller_impl,
      ModuleDataPtr module_data,
      fidl::InterfaceHandle<app::ServiceProvider> outgoing_services,
      fidl::InterfaceRequest<app::ServiceProvider> incoming_services,
      fidl::InterfaceRequest<ModuleController> module_controller_request,
      fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
      ResultCall result_call)
      : Operation("StoryControllerImpl::GetLedgerNotificationCall",
                  container,
                  std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)),
        outgoing_services_(std::move(outgoing_services)),
        incoming_services_(std::move(incoming_services)),
        module_controller_request_(std::move(module_controller_request)),
        view_owner_request_(std::move(view_owner_request)) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};
    auto i = std::find_if(
        story_controller_impl_->connections_.begin(),
        story_controller_impl_->connections_.end(),
        [this](const Connection& c) {
          return c.module_data->module_path.Equals(module_data_->module_path);
        });

    // We launch the new module if it doesn't run yet.
    if (i == story_controller_impl_->connections_.end()) {
      Launch(flow);
      return;
    }

    // If the new module is already running, but with a different URL or on a
    // different link, or if a service exchange is requested, we tear it down
    // then launch a new module.
    //
    // TODO(mesch): If only the link is different, we should just hook the
    // existing module instance on a new link and notify it about the changed
    // link value.
    if (i->module_data->module_url != module_data_->module_url ||
        !i->module_data->link_path->Equals(*module_data_->link_path) ||
        outgoing_services_.is_valid() || incoming_services_.is_pending()) {
      i->module_controller_impl->Teardown([this, flow] {
        // NOTE(mesch): i is invalid at this point.
        Launch(flow);
      });
      return;
    }

    // If the module is already running on the same URL and link, we just
    // connect the module controller request, if there is one. Modules started
    // with StoryController.AddModule() don't have a module controller request.
    if (module_controller_request_.is_pending()) {
      i->module_controller_impl->Connect(std::move(module_controller_request_));
    }
  }

  void Launch(FlowToken /*flow*/) {
    FXL_LOG(INFO) << "StoryControllerImpl::LaunchModule() "
                  << module_data_->module_url;
    auto module_config = AppConfig::New();
    module_config->url = module_data_->module_url;

    mozart::ViewProviderPtr view_provider;
    fidl::InterfaceRequest<mozart::ViewProvider> view_provider_request =
        view_provider.NewRequest();
    view_provider->CreateView(std::move(view_owner_request_), nullptr);

    app::ServiceProviderPtr provider;
    auto provider_request = provider.NewRequest();
    auto module_context = app::ConnectToService<ModuleContext>(provider.get());
    auto service_list = app::ServiceList::New();
    service_list->names.push_back(ModuleContext::Name_);
    service_list->provider = std::move(provider);

    Connection connection;
    connection.module_data = module_data_.Clone();

    connection.module_controller_impl = std::make_unique<ModuleControllerImpl>(
        story_controller_impl_,
        story_controller_impl_->story_scope_.GetLauncher(),
        std::move(module_config), connection.module_data.get(),
        std::move(service_list), std::move(module_context),
        std::move(view_provider_request), std::move(outgoing_services_),
        std::move(incoming_services_));

    // Modules started with StoryController.AddModule() don't have a module
    // controller request.
    if (module_controller_request_.is_pending()) {
      connection.module_controller_impl->Connect(
          std::move(module_controller_request_));
    }

    ModuleContextInfo module_context_info = {
        story_controller_impl_->story_provider_impl_->component_context_info(),
        story_controller_impl_,
        story_controller_impl_->story_provider_impl_
            ->user_intelligence_provider(),
        story_controller_impl_->story_provider_impl_->module_resolver()};

    connection.module_context_impl = std::make_unique<ModuleContextImpl>(
        module_context_info, connection.module_data.get(),
        connection.module_controller_impl.get(), std::move(provider_request));

    story_controller_impl_->connections_.emplace_back(std::move(connection));

    story_controller_impl_->watchers_.ForAllPtrs(
        [this](StoryWatcher* const watcher) {
          watcher->OnModuleAdded(module_data_.Clone());
        });

    story_controller_impl_->modules_watchers_.ForAllPtrs(
        [this](StoryModulesWatcher* const watcher) {
          watcher->OnNewModule(module_data_.Clone());
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  ModuleDataPtr module_data_;
  fidl::InterfaceHandle<app::ServiceProvider> outgoing_services_;
  fidl::InterfaceRequest<app::ServiceProvider> incoming_services_;
  fidl::InterfaceRequest<ModuleController> module_controller_request_;
  fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LaunchModuleCall);
};

class StoryControllerImpl::KillModuleCall : Operation<> {
 public:
  KillModuleCall(OperationContainer* const container,
                 StoryControllerImpl* const story_controller_impl,
                 ModuleDataPtr module_data,
                 const std::function<void()>& done)
      : Operation("StoryControllerImpl::KillModuleCall", container, [] {}),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)),
        done_(done) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};
    // If the module is external, we also notify story shell about it going
    // away. An internal module is stopped by its parent module, and it's up to
    // the parent module to defocus it first. TODO(mesch): Why not always
    // defocus?
    if (story_controller_impl_->story_shell_ &&
        module_data_->module_source == ModuleSource::EXTERNAL) {
      story_controller_impl_->story_shell_->DefocusView(
          PathString(module_data_->module_path), [this, flow] { Cont(flow); });
    } else {
      Cont(flow);
    }
  }

  void Cont(FlowToken flow) {
    // Teardown the module, which discards the module controller. A parent
    // module can call ModuleController.Stop() multiple times before the
    // ModuleController connection gets disconnected by Teardown(). Therefore,
    // this StopModuleCall Operation will cause the calls to be queued.
    // The first Stop() will cause the ModuleController to be closed, and
    // so subsequent Stop() attempts will not find a controller and will return.
    auto ii = std::find_if(
        story_controller_impl_->connections_.begin(),
        story_controller_impl_->connections_.end(),
        [this](const Connection& c) {
          return c.module_data->module_path.Equals(module_data_->module_path);
        });

    if (ii == story_controller_impl_->connections_.end()) {
      FXL_LOG(INFO) << "No ModuleController for Module"
                    << " " << PathString(module_data_->module_path) << ". "
                    << "Was ModuleContext.Stop() called twice?";
      done_();
      return;
    }

    // done_() must be called BEFORE the Teardown() done callback returns. See
    // comment in StopModuleCall::Kill() before making changes here. Be aware
    // that done_ is NOT the Done() callback of the Operation.
    ii->module_controller_impl->Teardown([this, flow] {
      Cont1(flow);
      done_();
    });
  }

  void Cont1(FlowToken /*flow*/) {
    story_controller_impl_->modules_watchers_.ForAllPtrs(
        [this](StoryModulesWatcher* const watcher) {
          watcher->OnStopModule(module_data_.Clone());
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  ModuleDataPtr module_data_;
  std::function<void()> done_;

  FXL_DISALLOW_COPY_AND_ASSIGN(KillModuleCall);
};

class StoryControllerImpl::StartModuleCall : Blockable, Operation<> {
 public:
  StartModuleCall(
      OperationContainer* const container,
      StoryControllerImpl* const story_controller_impl,
      const fidl::Array<fidl::String>& parent_module_path,
      const fidl::Array<fidl::String>& module_path,
      const fidl::String& module_url,
      const fidl::String& link_name,
      const ModuleSource module_source,
      SurfaceRelationPtr surface_relation,
      fidl::InterfaceHandle<app::ServiceProvider> outgoing_services,
      fidl::InterfaceRequest<app::ServiceProvider> incoming_services,
      fidl::InterfaceRequest<ModuleController> module_controller_request,
      fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
      ResultCall result_call)
      : Blockable(story_controller_impl),
        Operation("StoryControllerImpl::StartModuleCall",
                  container,
                  std::move(result_call),
                  module_url),
        story_controller_impl_(story_controller_impl),
        parent_module_path_(parent_module_path.Clone()),
        module_path_(module_path.Clone()),
        module_url_(module_url),
        link_name_(link_name),
        module_source_(module_source),
        surface_relation_(std::move(surface_relation)),
        outgoing_services_(std::move(outgoing_services)),
        incoming_services_(std::move(incoming_services)),
        module_controller_request_(std::move(module_controller_request)),
        view_owner_request_(std::move(view_owner_request)) {
    FXL_DCHECK(!parent_module_path_.is_null());

    Ready();
  }

 private:
  void Run() override {
    // NOTE(alhaad): We don't use flow tokens for blockable operations. See
    // class comment for |Blockable| to know why.

    // We currently require a 1:1 relationship between module
    // application instances and Module service instances, because
    // flutter only allows one ViewOwner per flutter application,
    // and we need one ViewOwner instance per Module instance.

    if (link_name_) {
      link_path_ = LinkPath::New();
      link_path_->module_path = parent_module_path_.Clone();
      link_path_->link_name = link_name_;
      Cont1();

    } else {
      // If the link name is null, this module receives the default link of its
      // parent module. We need to retrieve which one it is from story storage.
      new ReadDataCall<ModuleData>(
          &operation_queue_, story_controller_impl_->page(),
          MakeModuleKey(parent_module_path_), false /* not_found_is_ok */,
          XdrModuleData, [this](ModuleDataPtr module_data) {
            FXL_DCHECK(module_data);
            link_path_ = module_data->link_path.Clone();
            Cont1();
          });
    }
  }

  void Cont1() {
    module_data_ = ModuleData::New();
    module_data_->module_url = module_url_;
    module_data_->module_path = module_path_.Clone();
    module_data_->link_path = link_path_.Clone();
    module_data_->module_source = module_source_;
    module_data_->surface_relation = surface_relation_.Clone();
    module_data_->module_stopped = false;

    // We check if the data in the ledger is already what we want. If so, we do
    // nothing.
    // Read the module data.
    new ReadDataCall<ModuleData>(
        &operation_queue_, story_controller_impl_->page(),
        MakeModuleKey(module_path_), false /* not_found_is_ok */, XdrModuleData,
        [this](ModuleDataPtr data) {
          // If what we're about to write is already present on the ledger, just
          // launch the module.
          if (data.Equals(module_data_)) {
            Launch();
            return;
          }
          WriteModuleData();
        });
  }

  void WriteModuleData() {
    const std::string key{MakeModuleKey(module_path_)};
    new WriteDataCall<ModuleData>(
        &operation_queue_, story_controller_impl_->page(), key, XdrModuleData,
        module_data_.Clone(),
        [this] { Block(module_data_.Clone(), [this] { Launch(); }); });
  }

  void Launch() {
    new LaunchModuleCall(&operation_queue_, story_controller_impl_,
                         std::move(module_data_), std::move(outgoing_services_),
                         std::move(incoming_services_),
                         std::move(module_controller_request_),
                         std::move(view_owner_request_), [this] { Done(); });
  }

  // Passed in:
  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::Array<fidl::String> parent_module_path_;
  const fidl::Array<fidl::String> module_path_;
  const fidl::String module_url_;
  const fidl::String link_name_;
  const ModuleSource module_source_;
  const SurfaceRelationPtr surface_relation_;
  fidl::InterfaceHandle<app::ServiceProvider> outgoing_services_;
  fidl::InterfaceRequest<app::ServiceProvider> incoming_services_;
  fidl::InterfaceRequest<ModuleController> module_controller_request_;
  fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request_;

  LinkPathPtr link_path_;
  ModuleDataPtr module_data_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartModuleCall);
};

class StoryControllerImpl::StartModuleInShellCall : Operation<> {
 public:
  StartModuleInShellCall(
      OperationContainer* const container,
      StoryControllerImpl* const story_controller_impl,
      const fidl::Array<fidl::String>& parent_module_path,
      const fidl::String& module_name,
      const fidl::String& module_url,
      const fidl::String& link_name,
      fidl::InterfaceHandle<app::ServiceProvider> outgoing_services,
      fidl::InterfaceRequest<app::ServiceProvider> incoming_services,
      fidl::InterfaceRequest<ModuleController> module_controller_request,
      SurfaceRelationPtr surface_relation,
      const bool focus,
      ModuleSource module_source,
      ResultCall result_call)
      : Operation("StoryControllerImpl::StartModuleInShellCall",
                  container,
                  std::move(result_call),
                  module_url),
        story_controller_impl_(story_controller_impl),
        parent_module_path_(parent_module_path.Clone()),
        module_name_(module_name),
        module_url_(module_url),
        link_name_(link_name),
        outgoing_services_(std::move(outgoing_services)),
        incoming_services_(std::move(incoming_services)),
        module_controller_request_(std::move(module_controller_request)),
        surface_relation_(std::move(surface_relation)),
        focus_(focus),
        module_source_(module_source) {
    module_path_ = parent_module_path_.Clone();
    module_path_.push_back(module_name_);

    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    // TODO(mesch): The StartModuleCall may result in just a new
    // ModuleController connection to an existing
    // ModuleControllerImpl. In that case, the view owner request is
    // closed, and the view owner should not be sent to the story
    // shell.

    new StartModuleCall(
        &operation_queue_, story_controller_impl_, parent_module_path_,
        module_path_, module_url_, link_name_, module_source_,
        surface_relation_.Clone(), std::move(outgoing_services_),
        std::move(incoming_services_), std::move(module_controller_request_),
        view_owner_.NewRequest(), [this, flow] { Cont(flow); });
  }

  void Cont(FlowToken flow) {
    const fidl::String view_id = PathString(module_path_);

    // If this is called during Stop(), story_shell_ might already have been
    // reset. TODO(mesch): Then the whole operation should fail.
    if (story_controller_impl_->story_shell_) {
      const fidl::String parent_view_id = PathString(parent_module_path_);

      // We only add a module to story shell if its either a root module or its
      // parent is already known to story shell.
      if (parent_view_id == "" ||
          story_controller_impl_->connected_views_.count(parent_view_id)) {
        story_controller_impl_->story_shell_->ConnectView(
            std::move(view_owner_), view_id, parent_view_id,
            std::move(surface_relation_));
        story_controller_impl_->connected_views_.insert(view_id);
        story_controller_impl_->ProcessPendingViews();
        if (focus_) {
          story_controller_impl_->story_shell_->FocusView(view_id,
                                                          parent_view_id);
        }
      } else {
        // TODO(alhaad): We need to remove this if the module is stopped while
        // its view is still on this buffer.
        story_controller_impl_->pending_views_.emplace(std::make_pair(
            view_id.get(),
            ModuleView{parent_view_id.get(), std::move(view_owner_),
                       std::move(surface_relation_)}));
      }
    }
  }

  StoryControllerImpl* const story_controller_impl_;
  const fidl::Array<fidl::String> parent_module_path_;
  const fidl::String module_name_;
  const fidl::String module_url_;
  const fidl::String link_name_;
  fidl::InterfaceHandle<app::ServiceProvider> outgoing_services_;
  fidl::InterfaceRequest<app::ServiceProvider> incoming_services_;
  fidl::InterfaceRequest<ModuleController> module_controller_request_;
  SurfaceRelationPtr surface_relation_;
  const bool focus_;
  const ModuleSource module_source_;

  fidl::Array<fidl::String> module_path_;
  ModuleControllerPtr module_controller_;
  mozart::ViewOwnerPtr view_owner_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartModuleInShellCall);
};

class StoryControllerImpl::AddModuleCall : Blockable, Operation<> {
 public:
  AddModuleCall(OperationContainer* const container,
                StoryControllerImpl* const story_controller_impl,
                fidl::Array<fidl::String> parent_module_path,
                const fidl::String& module_name,
                const fidl::String& module_url,
                const fidl::String& link_name,
                SurfaceRelationPtr surface_relation,
                const ResultCall& done)
      : Blockable(story_controller_impl),
        Operation("StoryControllerImpl::AddModuleCall",
                  container,
                  done,
                  module_url),
        story_controller_impl_(story_controller_impl),
        parent_module_path_(std::move(parent_module_path)),
        module_name_(module_name),
        module_url_(module_url),
        link_name_(link_name),
        surface_relation_(std::move(surface_relation)) {
    Ready();
  }

 private:
  void Run() override {
    // NOTE(alhaad): We don't use flow tokens for blockable operations. See
    // class comment for |Blockable| to know why.
    module_path_ = parent_module_path_.Clone();
    module_path_.push_back(module_name_);
    link_path_ = LinkPath::New();
    link_path_->module_path = parent_module_path_.Clone();
    link_path_->link_name = link_name_;

    WriteModuleData();
  }

  void WriteModuleData() {
    module_data_ = ModuleData::New();
    module_data_->module_url = module_url_;
    module_data_->module_path = module_path_.Clone();
    module_data_->link_path = link_path_.Clone();
    module_data_->module_source = ModuleSource::EXTERNAL;
    module_data_->surface_relation = surface_relation_.Clone();
    module_data_->module_stopped = false;

    const std::string key{MakeModuleKey(module_path_)};
    new WriteDataCall<ModuleData>(
        &operation_queue_, story_controller_impl_->page(), key, XdrModuleData,
        module_data_.Clone(),
        [this] { Block(module_data_.Clone(), [this] { Cont(); }); });
  }

  void Cont() {
    if (story_controller_impl_->IsRunning()) {
      new StartModuleInShellCall(&operation_queue_, story_controller_impl_,
                                 parent_module_path_, module_name_, module_url_,
                                 link_name_, nullptr, nullptr, nullptr,
                                 std::move(surface_relation_), true,
                                 ModuleSource::EXTERNAL, [this] { Done(); });
    } else {
      Done();
    }
  }

  StoryControllerImpl* const story_controller_impl_;
  const fidl::Array<fidl::String> parent_module_path_;
  const fidl::String module_name_;
  const fidl::String module_url_;
  const fidl::String link_name_;
  SurfaceRelationPtr surface_relation_;

  fidl::Array<fidl::String> module_path_;
  LinkPathPtr link_path_;

  ModuleDataPtr module_data_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AddModuleCall);
};

class StoryControllerImpl::AddForCreateCall : Operation<> {
 public:
  AddForCreateCall(OperationContainer* const container,
                   StoryControllerImpl* const story_controller_impl,
                   const fidl::String& module_name,
                   const fidl::String& module_url,
                   const fidl::String& link_name,
                   const fidl::String& link_json,
                   const ResultCall& done)
      : Operation("StoryControllerImpl::AddForCreateCall",
                  container,
                  done,
                  module_url),
        story_controller_impl_(story_controller_impl),
        module_name_(module_name),
        module_url_(module_url),
        link_name_(link_name),
        link_json_(link_json) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    // This flow branches and then joins on all the branches completing, which
    // is just fine to track with a flow token. A callback like used below:
    //
    //  [flow] {}
    //
    // just calls Done() when the last copy of it completes.

    if (!link_json_.is_null()) {
      // There is no module path; this link exists outside the scope of a
      // module.
      LinkPathPtr link_path = LinkPath::New();
      link_path->module_path = fidl::Array<fidl::String>::New(0);
      link_path->link_name = link_name_;
      story_controller_impl_->ConnectLinkPath(std::move(link_path),
                                              link_.NewRequest());
      link_->UpdateObject(nullptr, link_json_);
      link_->Sync([flow] {});
    }

    new AddModuleCall(&operation_collection_, story_controller_impl_,
                      fidl::Array<fidl::String>::New(0), module_name_,
                      module_url_, link_name_, SurfaceRelation::New(),
                      [flow] {});
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::String module_name_;
  const fidl::String module_url_;
  const fidl::String link_name_;
  const fidl::String link_json_;

  LinkPtr link_;

  OperationCollection operation_collection_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AddForCreateCall);
};

class StoryControllerImpl::StopCall : Operation<> {
 public:
  StopCall(OperationContainer* const container,
           StoryControllerImpl* const story_controller_impl,
           const bool notify,
           std::function<void()> done)
      : Operation("StoryControllerImpl::StopCall", container, done),
        story_controller_impl_(story_controller_impl),
        notify_(notify) {
    Ready();
  }

 private:
  // StopCall may be run even on a story impl that is not running.
  void Run() override {
    // At this point, we don't need to monitor the root modules for state
    // changes anymore, because the next state change of the story is triggered
    // by the Cleanup() call below.
    story_controller_impl_->track_root_module_state_ = false;

    // At this point, we don't need notifications from disconnected
    // Links anymore, as they will all be disposed soon anyway.
    for (auto& link : story_controller_impl_->links_) {
      link->set_orphaned_handler(nullptr);
    }

    // Tear down all connections with a ModuleController first, then the
    // links between them.
    connections_count_ = story_controller_impl_->connections_.size();

    if (connections_count_ == 0) {
      StopStoryShell();
    } else {
      for (auto& connection : story_controller_impl_->connections_) {
        connection.module_controller_impl->Teardown(
            [this] { ConnectionDown(); });
      }
    }
  }

  void ConnectionDown() {
    --connections_count_;
    if (connections_count_ > 0) {
      // Not the last call.
      return;
    }

    StopStoryShell();
  }

  void StopStoryShell() {
    // It StopCall runs on a story that's not running, there is no story shell.
    if (story_controller_impl_->story_shell_) {
      story_controller_impl_->story_shell_.set_connection_error_handler(
          [this] { StoryShellDown(); });
      story_controller_impl_->story_shell_->Terminate();
    } else {
      StoryShellDown();
    }
  }

  void StoryShellDown() {
    story_controller_impl_->story_shell_controller_.reset();
    story_controller_impl_->story_shell_.reset();
    if (story_controller_impl_->story_context_binding_.is_bound()) {
      // Close() dchecks if called while not bound.
      story_controller_impl_->story_context_binding_.Close();
    }
    StopLinks();
  }

  void StopLinks() {
    links_count_ = story_controller_impl_->links_.size();
    if (links_count_ == 0) {
      Cleanup();
      return;
    }

    // The links don't need to be written now, because they all were written
    // when they were last changed, but we need to wait for the last write
    // request to finish, which is done with the Sync() request below.
    for (auto& link : story_controller_impl_->links_) {
      link->Sync([this] { LinkDown(); });
    }
  }

  void LinkDown() {
    --links_count_;
    if (links_count_ > 0) {
      // Not the last call.
      return;
    }

    Cleanup();
  }

  void Cleanup() {
    // Clear the remaining links and connections in case there are some left. At
    // this point, no DisposeLink() calls can arrive anymore.
    story_controller_impl_->links_.clear();
    story_controller_impl_->connections_.clear();

    story_controller_impl_->state_ = StoryState::STOPPED;

    // If this StopCall is part of a DeleteCall, then we don't notify story
    // state changes; the pertinent state change will be the delete notification
    // instead.
    if (notify_) {
      story_controller_impl_->NotifyStateChange();
    }

    Done();
  };

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const bool notify_;  // Whether to notify state change; false in DeleteCall.
  int connections_count_{};
  int links_count_{};

  FXL_DISALLOW_COPY_AND_ASSIGN(StopCall);
};

class StoryControllerImpl::StopModuleCall : Blockable, Operation<> {
 public:
  StopModuleCall(OperationContainer* const container,
                 StoryControllerImpl* const story_controller_impl,
                 const fidl::Array<fidl::String>& module_path,
                 const std::function<void()>& done)
      : Blockable(story_controller_impl),
        Operation("StoryControllerImpl::StopModuleCall", container, done),
        story_controller_impl_(story_controller_impl),
        module_path_(module_path.Clone()) {
    Ready();
  }

 private:
  void Run() override {
    // NOTE(alhaad): We don't use flow tokens for blockable operations. See
    // class comment for |Blockable| to know why.

    // Read the module data.
    new ReadDataCall<ModuleData>(
        &operation_queue_, story_controller_impl_->page(),
        MakeModuleKey(module_path_), false /* not_found_is_ok */, XdrModuleData,
        [this](ModuleDataPtr data) {
          module_data_ = std::move(data);
          Cont1();
        });
  }

  void Cont1() {
    // If the module is already marked as stopped, kill module.
    if (module_data_->module_stopped) {
      Kill();
      return;
    }

    // Write the module data back, with module_stopped = true, which is a
    // global state shared between machines to track when the module is
    // explicitly stopped.
    module_data_->module_stopped = true;

    const std::string key{MakeModuleKey(module_data_->module_path)};
    new WriteDataCall<ModuleData>(
        &operation_queue_, story_controller_impl_->page(), key, XdrModuleData,
        module_data_->Clone(), [this] {
          // TODO(alhaad: This Block() call may never continue if the data
          // we're writing to the ledger is the same as the data already in
          // there as that will not trigger an OnPageChange().
          Block(module_data_.Clone(), [this] { Kill(); });
        });
  }

  void Kill() {
    new KillModuleCall(&operation_queue_, story_controller_impl_,
                       std::move(module_data_), [this] {
                         // NOTE(alhaad): An interesting flow of control to keep
                         // in mind:
                         // 1. From ModuleController.Stop() which can only be
                         // called from FIDL, we call
                         // StoryControllerImpl.StopModule().
                         // 2. StoryControllerImpl.StopModule() pushes
                         // StopModuleCall onto the operation queue.
                         // 3. When operation becomes current, we write to
                         // ledger, block and continue on receiving OnPageChange
                         // from ledger.
                         // 4. We then call KillModuleCall on a sub operation
                         // queue.
                         // 5. KillModuleCall will call Teardown() on the same
                         // ModuleControllerImpl that had started
                         // ModuleController.Stop(). In the callback from
                         // Teardown(), it calls done_() (and NOT Done()).
                         // 6. done_() in KillModuleCall leads to the next line
                         // here, which calls Done() which would call the FIDL
                         // callback from ModuleController.Stop().
                         // 7. Done() on the next line also deletes this which
                         // deletes the still running KillModuleCall, but this
                         // is okay because the only thing that was left to do
                         // in KillModuleCall was FlowToken going out of scope.
                         Done();
                       });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::Array<fidl::String> module_path_;
  ModuleDataPtr module_data_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StopModuleCall);
};

class StoryControllerImpl::DeleteCall : Operation<> {
 public:
  DeleteCall(OperationContainer* const container,
             StoryControllerImpl* const story_controller_impl,
             std::function<void()> done)
      : Operation("StoryControllerImpl::DeleteCall", container, [] {}),
        story_controller_impl_(story_controller_impl),
        done_(std::move(done)) {
    Ready();
  }

 private:
  void Run() override {
    // No call to Done(), in order to block all further operations on the queue
    // until the instance is deleted.
    new StopCall(&operation_queue_, story_controller_impl_, false /* notify */,
                 done_);
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned

  // Not the result call of the Operation, because it's invoked without
  // unblocking the operation queue, to prevent subsequent operations from
  // executing until the instance is deleted, which cancels those operations.
  std::function<void()> done_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DeleteCall);
};

class StoryControllerImpl::StartCall : Operation<> {
 public:
  StartCall(OperationContainer* const container,
            StoryControllerImpl* const story_controller_impl,
            fidl::InterfaceRequest<mozart::ViewOwner> request)
      : Operation("StoryControllerImpl::StartCall", container, [] {}),
        story_controller_impl_(story_controller_impl),
        request_(std::move(request)) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    // If the story is running, we do nothing and close the view owner request.
    if (story_controller_impl_->IsRunning()) {
      FXL_LOG(INFO)
          << "StoryControllerImpl::StartCall() while already running: ignored.";
      return;
    }

    story_controller_impl_->track_root_module_state_ = true;
    story_controller_impl_->StartStoryShell(std::move(request_));

    // Start *all* the root modules, not just the first one, with their
    // respective links.
    new ReadAllDataCall<ModuleData>(
        &operation_queue_, story_controller_impl_->page(), kModuleKeyPrefix,
        XdrModuleData, [this, flow](fidl::Array<ModuleDataPtr> data) {
          for (auto& module_data : data) {
            if (module_data->module_source == ModuleSource::EXTERNAL &&
                !module_data->module_stopped) {
              auto parent_path = module_data->module_path.Clone();
              parent_path.resize(parent_path.size() - 1);
              new StartModuleInShellCall(
                  &operation_queue_, story_controller_impl_, parent_path,
                  module_data->module_path[module_data->module_path.size() - 1],
                  module_data->module_url, module_data->link_path->link_name,
                  nullptr, nullptr, nullptr,
                  module_data->surface_relation.Clone(), true,
                  module_data->module_source, [flow] {});
            }
          }

          story_controller_impl_->state_ = StoryState::STARTING;
          story_controller_impl_->NotifyStateChange();
        });
  };

  StoryControllerImpl* const story_controller_impl_;  // not owned
  fidl::InterfaceRequest<mozart::ViewOwner> request_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartCall);
};

class StoryControllerImpl::GetImportanceCall : Operation<float> {
 public:
  GetImportanceCall(OperationContainer* const container,
                    StoryControllerImpl* const story_controller_impl,
                    const ContextState& context_state,
                    ResultCall result_call)
      : Operation("StoryControllerImpl::GetImportanceCall",
                  container,
                  std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        context_state_(context_state.Clone()) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &result_};

    new ReadAllDataCall<StoryContextLog>(
        &operation_queue_, story_controller_impl_->page(),
        kStoryContextLogKeyPrefix, XdrStoryContextLog,
        [this, flow](fidl::Array<StoryContextLogPtr> log) {
          log_ = std::move(log);
          Cont(flow);
        });
  }

  void Cont(FlowToken /*flow*/) {
    // HACK(mesch): Hardcoded importance computation. Will be delegated
    // somewhere more flexible eventually.
    auto i = context_state_.find(kStoryImportanceContext);
    if (i == context_state_.cend()) {
      result_ = 1.0;
      return;
    }

    const auto& context_value = i.GetValue();

    float score = 0.0;
    float count = 0.0;

    for (auto& entry : log_) {
      auto i = entry->context.find(kStoryImportanceContext);
      if (i == entry->context.end()) {
        continue;
      }

      // Any log entry with context relevant to importance counts.
      count += 1.0;

      const auto& log_value = i.GetValue();
      if (context_value != log_value) {
        continue;
      }

      // Any log entry with context relevant to importance increases the
      // importance score.
      score += 1.0;
    }

    if (count > 0.0) {
      result_ = score / count;
    }
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const ContextState context_state_;
  fidl::Array<StoryContextLogPtr> log_;

  float result_{0.0};

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GetImportanceCall);
};

class StoryControllerImpl::LedgerNotificationCall : Operation<> {
 public:
  LedgerNotificationCall(OperationContainer* const container,
                         StoryControllerImpl* const story_controller_impl,
                         ModuleDataPtr module_data)
      : Operation("StoryControllerImpl::LedgerNotificationCall",
                  container,
                  [] {}),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};
    if (!story_controller_impl_->IsRunning() ||
        module_data_->module_source != ModuleSource::EXTERNAL) {
      return;
    }

    // Check for existing module at the given path.
    auto i = std::find_if(
        story_controller_impl_->connections_.begin(),
        story_controller_impl_->connections_.end(),
        [this](const Connection& c) {
          return c.module_data->module_path.Equals(module_data_->module_path);
        });
    if (i != story_controller_impl_->connections_.end() &&
        module_data_->module_stopped) {
      new KillModuleCall(&operation_queue_, story_controller_impl_,
                         std::move(module_data_), [flow] {});
      return;
    } else if (module_data_->module_stopped) {
      // There is no module running, and the ledger change is for a stopped
      // module so do nothing.
      return;
    }

    // We reach this point only if we want to start an external module.
    auto parent_path = module_data_->module_path.Clone();
    parent_path.resize(parent_path.size() - 1);
    new StartModuleInShellCall(
        &operation_queue_, story_controller_impl_, parent_path,
        module_data_->module_path[module_data_->module_path.size() - 1],
        module_data_->module_url, module_data_->link_path->link_name, nullptr,
        nullptr, nullptr, std::move(module_data_->surface_relation), true,
        module_data_->module_source, [flow] {});
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;  // not owned
  ModuleDataPtr module_data_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerNotificationCall);
};

StoryControllerImpl::StoryControllerImpl(
    const fidl::String& story_id,
    LedgerClient* const ledger_client,
    LedgerPageId story_page_id,
    StoryProviderImpl* const story_provider_impl)
    : PageClient(MakeStoryKey(story_id),
                 ledger_client,
                 story_page_id.Clone(),
                 kModuleKeyPrefix),
      story_id_(story_id),
      story_provider_impl_(story_provider_impl),
      ledger_client_(ledger_client),
      story_page_id_(std::move(story_page_id)),
      story_scope_(story_provider_impl_->user_scope(),
                   kStoryScopeLabelPrefix + story_id_.get()),
      story_context_binding_(this),
      story_marker_impl_(new StoryMarkerImpl) {
  story_scope_.AddService<StoryMarker>(
      [this](fidl::InterfaceRequest<StoryMarker> request) {
        story_marker_impl_->Connect(std::move(request));
      });

  auto story_scope = maxwell::StoryScope::New();
  story_scope->story_id = story_id;
  auto scope = maxwell::ComponentScope::New();
  scope->set_story_scope(std::move(story_scope));
  story_provider_impl_->user_intelligence_provider()
      ->GetComponentIntelligenceServices(std::move(scope),
                                         intelligence_services_.NewRequest());

  story_scope_.AddService<maxwell::ContextWriter>(
      [this](fidl::InterfaceRequest<maxwell::ContextWriter> request) {
        intelligence_services_->GetContextWriter(std::move(request));
      });
}

StoryControllerImpl::~StoryControllerImpl() = default;

void StoryControllerImpl::Connect(
    fidl::InterfaceRequest<StoryController> request) {
  bindings_.AddBinding(this, std::move(request));
}

bool StoryControllerImpl::IsRunning() {
  switch (state_) {
    case StoryState::STARTING:
    case StoryState::RUNNING:
    case StoryState::DONE:
      return true;
    case StoryState::INITIAL:
    case StoryState::STOPPED:
    case StoryState::ERROR:
      return false;
  }
}

void StoryControllerImpl::StopForDelete(const StopCallback& done) {
  new DeleteCall(&operation_queue_, this, done);
}

void StoryControllerImpl::StopForTeardown(const StopCallback& done) {
  new StopCall(&operation_queue_, this, false /* notify */, done);
}

void StoryControllerImpl::AddForCreate(const fidl::String& module_name,
                                       const fidl::String& module_url,
                                       const fidl::String& link_name,
                                       const fidl::String& link_json,
                                       const std::function<void()>& done) {
  new AddForCreateCall(&operation_queue_, this, module_name, module_url,
                       link_name, link_json, done);
}

StoryState StoryControllerImpl::GetStoryState() const {
  return state_;
}

void StoryControllerImpl::Log(StoryContextLogPtr log_entry) {
  new WriteDataCall<StoryContextLog>(
      &operation_queue_, page(),
      MakeStoryContextLogKey(log_entry->signal, log_entry->time),
      XdrStoryContextLog, std::move(log_entry), [] {});
}

void StoryControllerImpl::Sync(const std::function<void()>& done) {
  new SyncCall(&operation_queue_, done);
}

void StoryControllerImpl::GetImportance(
    const ContextState& context_state,
    const std::function<void(float)>& result) {
  new GetImportanceCall(&operation_queue_, this, context_state, result);
}

void StoryControllerImpl::FocusModule(
    const fidl::Array<fidl::String>& module_path) {
  if (story_shell_) {
    if (!module_path.empty()) {
      // Focus modules relative to their parent modules.
      fidl::Array<fidl::String> parent_module_path = module_path.Clone();
      parent_module_path.resize(parent_module_path.size() - 1);
      story_shell_->FocusView(PathString(module_path),
                              PathString(parent_module_path));
    } else {
      // Focus root modules absolutely.
      story_shell_->FocusView(PathString(module_path), nullptr);
    }
  }
}

void StoryControllerImpl::DefocusModule(
    const fidl::Array<fidl::String>& module_path) {
  if (story_shell_) {
    story_shell_->DefocusView(PathString(module_path), [] {});
  }
}

void StoryControllerImpl::StopModule(
    const fidl::Array<fidl::String>& module_path,
    const std::function<void()>& done) {
  new StopModuleCall(&operation_queue_, this, module_path, done);
}

void StoryControllerImpl::ReleaseModule(
    ModuleControllerImpl* const module_controller_impl) {
  auto f = std::find_if(connections_.begin(), connections_.end(),
                        [module_controller_impl](const Connection& c) {
                          return c.module_controller_impl.get() ==
                                 module_controller_impl;
                        });
  FXL_DCHECK(f != connections_.end());
  f->module_controller_impl.release();
  connections_.erase(f);
}

const fidl::String& StoryControllerImpl::GetStoryId() const {
  return story_id_;
}

void StoryControllerImpl::RequestStoryFocus() {
  story_provider_impl_->RequestStoryFocus(story_id_);
}

// TODO(vardhan): Should this operation be queued here, or in |LinkImpl|?
// Currently it is neither.
void StoryControllerImpl::ConnectLinkPath(
    LinkPathPtr link_path,
    fidl::InterfaceRequest<Link> request) {
  auto i = std::find_if(links_.begin(), links_.end(),
                        [&link_path](const std::unique_ptr<LinkImpl>& l) {
                          return l->link_path().Equals(link_path);
                        });
  if (i != links_.end()) {
    (*i)->Connect(std::move(request));
    return;
  }

  LinkImpl* const link_impl = new LinkImpl(
      ledger_client_, story_page_id_.Clone(), std::move(link_path));
  link_impl->Connect(std::move(request));
  links_.emplace_back(link_impl);
  link_impl->set_orphaned_handler(
      [this, link_impl] { DisposeLink(link_impl); });

  links_watchers_.ForAllPtrs([link_impl](StoryLinksWatcher* const watcher) {
    watcher->OnNewLink(link_impl->link_path().Clone());
  });
}

void StoryControllerImpl::StartModule(
    const fidl::Array<fidl::String>& parent_module_path,
    const fidl::String& module_name,
    const fidl::String& module_url,
    const fidl::String& link_name,
    fidl::InterfaceHandle<app::ServiceProvider> outgoing_services,
    fidl::InterfaceRequest<app::ServiceProvider> incoming_services,
    fidl::InterfaceRequest<ModuleController> module_controller_request,
    fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
    const ModuleSource module_source) {
  fidl::Array<fidl::String> module_path = parent_module_path.Clone();
  module_path.push_back(module_name);
  new StartModuleCall(&operation_queue_, this, parent_module_path, module_path,
                      module_url, link_name, module_source,
                      SurfaceRelation::New(), std::move(outgoing_services),
                      std::move(incoming_services),
                      std::move(module_controller_request),
                      std::move(view_owner_request), [] {});
}

void StoryControllerImpl::StartModuleInShell(
    const fidl::Array<fidl::String>& parent_module_path,
    const fidl::String& module_name,
    const fidl::String& module_url,
    const fidl::String& link_name,
    fidl::InterfaceHandle<app::ServiceProvider> outgoing_services,
    fidl::InterfaceRequest<app::ServiceProvider> incoming_services,
    fidl::InterfaceRequest<ModuleController> module_controller_request,
    SurfaceRelationPtr surface_relation,
    const bool focus,
    ModuleSource module_source) {
  new StartModuleInShellCall(
      &operation_queue_, this, parent_module_path, module_name, module_url,
      link_name, std::move(outgoing_services), std::move(incoming_services),
      std::move(module_controller_request), std::move(surface_relation), focus,
      module_source, [] {});
}

void StoryControllerImpl::ProcessPendingViews() {
  std::vector<std::string> added_keys;

  for (auto& kv : pending_views_) {
    if (!connected_views_.count(kv.second.parent_view_id)) {
      continue;
    }

    if (story_shell_) {
      auto& c = kv.second;
      story_shell_->ConnectView(std::move(c.view_owner), kv.first,
                                c.parent_view_id,
                                std::move(c.surface_relation));
      connected_views_.insert(kv.first);
      added_keys.push_back(kv.first);
    }
  }

  if (added_keys.size()) {
    for (auto& key : added_keys) {
      pending_views_.erase(key);
    }
    ProcessPendingViews();
  }
}

void StoryControllerImpl::OnPageChange(const std::string& key,
                                       const std::string& value) {
  FXL_LOG(INFO) << "StoryControllerImpl::OnPageChange " << key << " " << value;
  auto module_data = ModuleData::New();
  if (!XdrRead(value, &module_data, XdrModuleData)) {
    FXL_LOG(ERROR) << "Unable to parse ModuleData " << key << " " << value;
    return;
  }

  // Check if we already have a blocked operation for this update.
  auto i = std::find_if(blocked_operations_.begin(), blocked_operations_.end(),
                        [&module_data](const auto& p) {
                          return p.first.Equals(module_data);
                        });
  if (i != blocked_operations_.end()) {
    // For an already blocked operation, we simply continue the operation.
    auto op = i->second;
    blocked_operations_.erase(i);
    op->Continue();
    return;
  }

  // Control reaching here means that this update came from a remote device.
  new LedgerNotificationCall(&operation_queue_, this, std::move(module_data));
}

// |StoryController|
void StoryControllerImpl::GetInfo(const GetInfoCallback& callback) {
  // Synced such that if GetInfo() is called after Start() or Stop(), the state
  // after the previously invoked operation is returned.
  //
  // If this call enters a race with a StoryProvider.DeleteStory() call, it may
  // silently not return or return null, or return the story info before it was
  // deleted, depending on where it gets sequenced in the operation queues of
  // StoryControllerImpl and StoryProviderImpl. The queues do not block each
  // other, however, because the call on the second queue is made in the done
  // callback of the operation on the first queue.
  //
  // This race is normal fidl concurrency behavior.
  new SyncCall(&operation_queue_, [this, callback] {
    story_provider_impl_->GetStoryInfo(
        story_id_,
        // We capture only |state_| and not |this| because (1) we want the state
        // after SyncCall finishes, not after GetStoryInfo returns (i.e. we want
        // the state after the previous operation before GetInfo(), but not
        // after the operation following GetInfo()), and (2) |this| may have
        // been deleted when GetStoryInfo returned if there was a Delete
        // operation in the queue before GetStoryInfo().
        [state = state_, callback](modular::StoryInfoPtr story_info) {
          callback(std::move(story_info), state);
        });
  });
}

// |StoryController|
void StoryControllerImpl::SetInfoExtra(const fidl::String& name,
                                       const fidl::String& value,
                                       const SetInfoExtraCallback& callback) {
  story_provider_impl_->SetStoryInfoExtra(story_id_, name, value, callback);
}

// |StoryController|
void StoryControllerImpl::Start(
    fidl::InterfaceRequest<mozart::ViewOwner> request) {
  new StartCall(&operation_queue_, this, std::move(request));
}

// |StoryController|
void StoryControllerImpl::Stop(const StopCallback& done) {
  new StopCall(&operation_queue_, this, true /* notify */, done);
}

// |StoryController|
void StoryControllerImpl::Watch(fidl::InterfaceHandle<StoryWatcher> watcher) {
  auto ptr = StoryWatcherPtr::Create(std::move(watcher));
  ptr->OnStateChange(state_);
  watchers_.AddInterfacePtr(std::move(ptr));
}

// |StoryController|
void StoryControllerImpl::AddModule(fidl::Array<fidl::String> module_path,
                                    const fidl::String& module_name,
                                    const fidl::String& module_url,
                                    const fidl::String& link_name,
                                    SurfaceRelationPtr surface_relation) {
  // In the API, a null module path is allowed to represent the empty module
  // path.
  if (module_path.is_null()) {
    module_path.resize(0);
  }

  new AddModuleCall(&operation_queue_, this, std::move(module_path),
                    module_name, module_url, link_name,
                    std::move(surface_relation), [] {});
}

// |StoryController|
void StoryControllerImpl::GetActiveModules(
    fidl::InterfaceHandle<StoryModulesWatcher> watcher,
    const GetActiveModulesCallback& callback) {
  // We execute this in a SyncCall so that we are sure we don't fall in a crack
  // between a module being created and inserted in the connections collection
  // during some Operation.
  new SyncCall(
      &operation_queue_, fxl::MakeCopyable([this, watcher = std::move(watcher),
                                            callback]() mutable {
        if (watcher) {
          auto ptr = StoryModulesWatcherPtr::Create(std::move(watcher));
          modules_watchers_.AddInterfacePtr(std::move(ptr));
        }

        fidl::Array<ModuleDataPtr> result;
        result.resize(0);
        for (auto& connection : connections_) {
          result.push_back(connection.module_data.Clone());
        }
        callback(std::move(result));
      }));
}

// |StoryController|
void StoryControllerImpl::GetModules(const GetModulesCallback& callback) {
  new ReadAllDataCall<ModuleData>(&operation_queue_, page(), kModuleKeyPrefix,
                                  XdrModuleData, callback);
}

// |StoryController|
void StoryControllerImpl::GetModuleController(
    fidl::Array<fidl::String> module_path,
    fidl::InterfaceRequest<ModuleController> request) {
  new SyncCall(
      &operation_queue_,
      fxl::MakeCopyable([this, module_path = std::move(module_path),
                         request = std::move(request)]() mutable {
        for (auto& connection : connections_) {
          if (module_path.Equals(connection.module_data->module_path)) {
            connection.module_controller_impl->Connect(std::move(request));
            return;
          }
        }

        // Trying to get a controller for a module that is not active just
        // drops the connection request.
      }));
}

// |StoryController|
void StoryControllerImpl::GetActiveLinks(
    fidl::InterfaceHandle<StoryLinksWatcher> watcher,
    const GetActiveLinksCallback& callback) {
  // We execute this in a SyncCall so that we are sure we don't fall in a crack
  // between a link being created and inserted in the links collection during
  // some Operation. (Right now Links are not created in an Operation, but we
  // don't want to rely on it.)
  new SyncCall(&operation_queue_,
               fxl::MakeCopyable([this, watcher = std::move(watcher),
                                  callback]() mutable {
                 if (watcher) {
                   auto ptr = StoryLinksWatcherPtr::Create(std::move(watcher));
                   links_watchers_.AddInterfacePtr(std::move(ptr));
                 }

                 // Only active links, i.e. links currently in use by a module,
                 // are returned here. Eventually we might want to list all
                 // links, but this requires some changes to how links are
                 // stored to make it nice. (Right now we need to parse keys,
                 // which we don't want to.)
                 fidl::Array<LinkPathPtr> result;
                 result.resize(0);
                 for (auto& link : links_) {
                   result.push_back(link->link_path().Clone());
                 }
                 callback(std::move(result));
               }));
}

// |StoryController|
void StoryControllerImpl::GetLink(fidl::Array<fidl::String> module_path,
                                  const fidl::String& name,
                                  fidl::InterfaceRequest<Link> request) {
  // In the API, a null module path is allowed to represent the empty module
  // path.
  if (module_path.is_null()) {
    module_path.resize(0);
  }

  LinkPathPtr link_path = LinkPath::New();
  link_path->module_path = std::move(module_path);
  link_path->link_name = name;

  ConnectLinkPath(std::move(link_path), std::move(request));
}

void StoryControllerImpl::StartStoryShell(
    fidl::InterfaceRequest<mozart::ViewOwner> request) {
  story_shell_controller_ = story_provider_impl_->StartStoryShell(
      story_context_binding_.NewBinding(), story_shell_.NewRequest(),
      std::move(request));
}

void StoryControllerImpl::NotifyStateChange() {
  watchers_.ForAllPtrs(
      [this](StoryWatcher* const watcher) { watcher->OnStateChange(state_); });

  story_provider_impl_->NotifyStoryStateChange(story_id_, state_);

  // NOTE(mesch): This gets scheduled on the StoryControllerImpl Operation
  // queue. If the current StoryControllerImpl Operation is part of a
  // DeleteStory Operation of the StoryProviderImpl, then the SetStoryState
  // Operation gets scheduled after the delete of the story is completed, and it
  // will not execute because its queue is deleted beforehand.
  //
  // TODO(mesch): Maybe we should execute this inside the containing Operation.

  PerDeviceStoryInfoPtr data = PerDeviceStoryInfo::New();
  data->device_id = story_provider_impl_->device_id();
  data->story_id = story_id_;
  data->timestamp = time(nullptr);
  data->state = state_;

  new WriteDataCall<PerDeviceStoryInfo, PerDeviceStoryInfoPtr>(
      &operation_queue_, page(), MakePerDeviceKey(data->device_id),
      XdrPerDeviceStoryInfo, std::move(data), [] {});
}

void StoryControllerImpl::DisposeLink(LinkImpl* const link) {
  auto f = std::find_if(
      links_.begin(), links_.end(),
      [link](const std::unique_ptr<LinkImpl>& l) { return l.get() == link; });
  FXL_DCHECK(f != links_.end());
  links_.erase(f);
}

bool StoryControllerImpl::IsRootModule(
    const fidl::Array<fidl::String>& module_path) {
  return module_path.size() == 1 && module_path[0] == kRootModuleName;
}

bool StoryControllerImpl::IsExternalModule(
    const fidl::Array<fidl::String>& module_path) {
  auto i = std::find_if(connections_.begin(), connections_.end(),
                        [&module_path](const Connection& c) {
                          return c.module_data->module_path.Equals(module_path);
                        });

  if (i == connections_.end()) {
    return false;
  }

  return i->module_data->module_source == ModuleSource::EXTERNAL;
}

void StoryControllerImpl::OnModuleStateChange(
    const fidl::Array<fidl::String>& module_path,
    const ModuleState state) {
  if (!track_root_module_state_) {
    return;
  }

  if (IsRootModule(module_path)) {
    OnRootStateChange(state);
  }

  if (IsExternalModule(module_path) && state == ModuleState::DONE) {
    StopModule(module_path, [] {});
  }
}

void StoryControllerImpl::OnRootStateChange(const ModuleState state) {
  switch (state) {
    case ModuleState::STARTING:
      state_ = StoryState::STARTING;
      break;
    case ModuleState::RUNNING:
    case ModuleState::UNLINKED:
      state_ = StoryState::RUNNING;
      break;
    case ModuleState::STOPPED:
      // TODO(mesch): The story should only be marked STOPPED after
      // StoryContoller.Stop() is executed, and no modules are left running. In
      // this state here, there may be modules other than the root module left
      // running. These modules may even request more modules to start or make
      // suggestions to start more modules, which would be shown to the
      // user. However, the calls to run the modules would silently not result
      // in modules running, just in the modules to be added to the story
      // record, because actually starting newly added modules is gated by the
      // story to be running. This makes little sense. FW-334
      state_ = StoryState::STOPPED;
      break;
    case ModuleState::DONE:
      // TODO(mesch): Same problem for modules remaining running and for newly
      // added modules as for STOPPED. FW-334
      state_ = StoryState::DONE;
      break;
    case ModuleState::ERROR:
      state_ = StoryState::ERROR;
      break;
  }

  NotifyStateChange();
}

}  // namespace modular
