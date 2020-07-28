// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/lib/modular_config/modular_config_xdr.h"

#include "src/modular/lib/modular_config/modular_config_constants.h"

namespace modular {
namespace {

void XdrBaseShellConfig(XdrContext* const xdr,
                        fuchsia::modular::session::BaseShellConfig* const data) {
  bool has_url = data->mutable_app_config()->has_url();
  xdr->FieldWithDefault(modular_config::kUrl, data->mutable_app_config()->mutable_url(), has_url,
                        std::string(modular_config::kDefaultBaseShellUrl));

  bool has_keep_alive_after_login = data->has_keep_alive_after_login();
  xdr->FieldWithDefault(modular_config::kKeepAliveAfterLogin,
                        data->mutable_keep_alive_after_login(), has_keep_alive_after_login, false);

  bool has_args = data->mutable_app_config()->has_args();
  xdr->FieldWithDefault(modular_config::kArgs, data->mutable_app_config()->mutable_args(), has_args,
                        std::vector<std::string>());
}

fuchsia::ui::policy::DisplayUsage GetDisplayUsageFromString(std::string_view usage) {
  if (usage == modular_config::kHandheld) {
    return fuchsia::ui::policy::DisplayUsage::kHandheld;
  } else if (usage == modular_config::kClose) {
    return fuchsia::ui::policy::DisplayUsage::kClose;
  } else if (usage == modular_config::kNear) {
    return fuchsia::ui::policy::DisplayUsage::kNear;
  } else if (usage == modular_config::kMidrange) {
    return fuchsia::ui::policy::DisplayUsage::kMidrange;
  } else if (usage == modular_config::kFar) {
    return fuchsia::ui::policy::DisplayUsage::kFar;
  }

  return fuchsia::ui::policy::DisplayUsage::kUnknown;
}

std::string GetDisplayUsageAsString(fuchsia::ui::policy::DisplayUsage usage) {
  switch (usage) {
    case fuchsia::ui::policy::DisplayUsage::kUnknown:
      return modular_config::kUnknown;
    case fuchsia::ui::policy::DisplayUsage::kHandheld:
      return modular_config::kHandheld;
    case fuchsia::ui::policy::DisplayUsage::kClose:
      return modular_config::kClose;
    case fuchsia::ui::policy::DisplayUsage::kNear:
      return modular_config::kNear;
    case fuchsia::ui::policy::DisplayUsage::kMidrange:
      return modular_config::kMidrange;
    case fuchsia::ui::policy::DisplayUsage::kFar:
      return modular_config::kFar;
  }
}

void XdrSessionShellMapEntry(XdrContext* const xdr,
                             fuchsia::modular::session::SessionShellMapEntry* const data) {
  // The default name is empty.
  bool has_name = data->has_name();
  xdr->FieldWithDefault(modular_config::kName, data->mutable_name(), has_name, std::string());

  auto* config = data->mutable_config();

  bool has_display_usage = config->has_display_usage();
  bool has_screen_height = config->has_screen_height();
  bool has_screen_width = config->has_screen_width();

  std::string display_usage_str = has_display_usage
                                      ? GetDisplayUsageAsString(config->display_usage())
                                      : modular_config::kUnknown;

  // We need to manually parse any field in JSON that is a represented as a fidl
  // enum because XDR expects a number, rather than a string, for enums.
  // If writing, this will set the value of "display_usage" in JSON as the
  // value of |display_usage|. If reading, this will read the value of
  // "display_usage" into |display_usage|.
  std::string display_usage;
  xdr->FieldWithDefault(modular_config::kDisplayUsage, &display_usage, false, display_usage_str);

  // This is only used when reading. We the value read into |display_usage| into |data|.
  if (xdr->op() == XdrOp::FROM_JSON) {
    auto display_usage_fidl = GetDisplayUsageFromString(display_usage);
    config->set_display_usage(display_usage_fidl);
  }

  xdr->FieldWithDefault(modular_config::kScreenHeight, config->mutable_screen_height(),
                        has_screen_height, 0.f);

  xdr->FieldWithDefault(modular_config::kScreenWidth, config->mutable_screen_width(),
                        has_screen_width, 0.f);

  // AppConfig
  bool has_url = config->mutable_app_config()->has_url();
  xdr->FieldWithDefault(modular_config::kUrl, config->mutable_app_config()->mutable_url(), has_url,
                        std::string(modular_config::kDefaultSessionShellUrl));

  bool has_args = config->mutable_app_config()->has_args();
  xdr->FieldWithDefault(modular_config::kArgs, config->mutable_app_config()->mutable_args(),
                        has_args, std::vector<std::string>());
}

void XdrAppConfig(XdrContext* const xdr, fuchsia::modular::session::AppConfig* const data) {
  xdr->Field(modular_config::kUrl, data->mutable_url());

  bool has_args = data->has_args();
  std::vector<std::string> default_args;
  xdr->FieldWithDefault(modular_config::kArgs, data->mutable_args(), has_args, default_args);
}

void XdrComponentArgs(XdrContext* const xdr, fuchsia::modular::session::AppConfig* const data) {
  // TODO(fxbug.dev/55895): component_args should use "url" instead of "uri"
  xdr->Field(modular_config::kUri, data->mutable_url());

  bool has_args = data->has_args();
  std::vector<std::string> default_args;
  xdr->FieldWithDefault(modular_config::kArgs, data->mutable_args(), has_args, default_args);
}

void XdrAgentServiceIndexEntry(XdrContext* const xdr,
                               fuchsia::modular::session::AgentServiceIndexEntry* const data) {
  xdr->Field(modular_config::kServiceName, data->mutable_service_name());
  xdr->Field(modular_config::kAgentUrl, data->mutable_agent_url());
}

std::vector<fuchsia::modular::session::SessionShellMapEntry> GetDefaultSessionShellMap() {
  fuchsia::modular::session::SessionShellConfig config;
  config.mutable_app_config()->set_url(modular_config::kDefaultSessionShellUrl);
  config.mutable_app_config()->set_args(std::vector<std::string>());
  config.set_display_usage(fuchsia::ui::policy::DisplayUsage::kUnknown);
  config.set_screen_height(0);
  config.set_screen_width(0);

  fuchsia::modular::session::SessionShellMapEntry map_entry;
  map_entry.set_name(modular_config::kDefaultSessionShellUrl);
  map_entry.set_config(std::move(config));

  std::vector<fuchsia::modular::session::SessionShellMapEntry> session_shell_map(1);
  session_shell_map.at(0) = std::move(map_entry);

  return session_shell_map;
}

fuchsia::modular::session::BaseShellConfig GetDefaultBaseShellConfig() {
  fuchsia::modular::session::BaseShellConfig base_shell_config;
  base_shell_config.mutable_app_config()->set_url(modular_config::kDefaultBaseShellUrl);
  base_shell_config.mutable_app_config()->set_args(std::vector<std::string>());
  base_shell_config.set_keep_alive_after_login(false);

  return base_shell_config;
}

fuchsia::modular::session::BasemgrConfig GetDefaultBasemgrConfig() {
  rapidjson::Document doc;
  doc.SetObject();

  fuchsia::modular::session::BasemgrConfig config;
  auto ok = XdrRead(&doc, &config, XdrBasemgrConfig);
  FX_DCHECK(ok);

  return config;
}

fuchsia::modular::session::SessionmgrConfig GetDefaultSessionmgrConfig() {
  rapidjson::Document doc;
  doc.SetObject();

  fuchsia::modular::session::SessionmgrConfig config;
  auto ok = XdrRead(&doc, &config, XdrSessionmgrConfig);
  FX_DCHECK(ok);

  return config;
}

}  // namespace

void XdrModularConfig_v1(XdrContext* const xdr,
                         fuchsia::modular::session::ModularConfig* const data) {
  bool has_basemgr_config = data->has_basemgr_config();
  xdr->FieldWithDefault(modular_config::kBasemgrConfigName, data->mutable_basemgr_config(),
                        XdrBasemgrConfig_v1, has_basemgr_config, GetDefaultBasemgrConfig());

  bool has_sessionmgr_config = data->has_sessionmgr_config();
  xdr->FieldWithDefault(modular_config::kSessionmgrConfigName, data->mutable_sessionmgr_config(),
                        XdrSessionmgrConfig_v1, has_sessionmgr_config,
                        GetDefaultSessionmgrConfig());
}

void XdrBasemgrConfig_v1(XdrContext* const xdr,
                         fuchsia::modular::session::BasemgrConfig* const data) {
  bool has_enable_cobalt = data->has_enable_cobalt();
  xdr->FieldWithDefault(modular_config::kEnableCobalt, data->mutable_enable_cobalt(),
                        has_enable_cobalt, true);

  bool has_use_session_shell_for_story_shell_factory =
      data->has_use_session_shell_for_story_shell_factory();
  xdr->FieldWithDefault(modular_config::kUseSessionShellForStoryShellFactory,
                        data->mutable_use_session_shell_for_story_shell_factory(),
                        has_use_session_shell_for_story_shell_factory, false);

  // If no base shell is specified, all fields will be populated from the
  // default |base_shell_config|. Otherwise, the filter |XdrBaseShellConfig|
  // will fill in individual fields with default values.
  auto base_shell_config = GetDefaultBaseShellConfig();
  bool has_base_shell = data->has_base_shell();
  xdr->FieldWithDefault(modular_config::kBaseShell, data->mutable_base_shell(), XdrBaseShellConfig,
                        has_base_shell, std::move(base_shell_config));

  // If no session shells are specified, a default session shell will be
  // added to |data->session_shell_map|. Otherwise, the filter
  // |XdrSessionShellMapEntry| will fill in individual fields of each session
  // shell.
  auto session_shell_config = GetDefaultSessionShellMap();
  bool has_nonempty_session_shell_map =
      data->has_session_shell_map() && !data->session_shell_map().empty();
  xdr->FieldWithDefault(modular_config::kSessionShells, data->mutable_session_shell_map(),
                        XdrSessionShellMapEntry, has_nonempty_session_shell_map,
                        std::move(session_shell_config));

  bool has_story_shell_url = false;
  if (data->has_story_shell()) {
    if (data->story_shell().has_app_config()) {
      has_story_shell_url = data->story_shell().app_config().has_url();
    }
  }
  xdr->FieldWithDefault(modular_config::kStoryShellUrl,
                        data->mutable_story_shell()->mutable_app_config()->mutable_url(),
                        has_story_shell_url, std::string(modular_config::kDefaultStoryShellUrl));
  if (xdr->op() == XdrOp::FROM_JSON) {
    data->mutable_story_shell()->mutable_app_config()->set_args(std::vector<std::string>());
  }

  if (xdr->HasField(modular_config::kSessionLauncher, data->has_session_launcher())) {
    xdr->Field(modular_config::kSessionLauncher, data->mutable_session_launcher(), XdrAppConfig);
  } else {
    data->clear_session_launcher();
  }
}

void XdrSessionmgrConfig_v1(XdrContext* const xdr,
                            fuchsia::modular::session::SessionmgrConfig* const data) {
  bool has_enable_cobalt = data->has_enable_cobalt();
  xdr->FieldWithDefault(modular_config::kEnableCobalt, data->mutable_enable_cobalt(),
                        has_enable_cobalt, true);

  std::vector<std::string> default_agents;
  bool has_startup_agents = data->has_startup_agents();
  xdr->FieldWithDefault(modular_config::kStartupAgents, data->mutable_startup_agents(),
                        has_startup_agents, default_agents);

  bool has_session_agents = data->has_session_agents();
  xdr->FieldWithDefault(modular_config::kSessionAgents, data->mutable_session_agents(),
                        has_session_agents, default_agents);

  bool has_restart_session_on_agent_crash = data->has_restart_session_on_agent_crash();
  xdr->FieldWithDefault(modular_config::kRestartSessionOnAgentCrash,
                        data->mutable_restart_session_on_agent_crash(),
                        has_restart_session_on_agent_crash, default_agents);

  std::vector<fuchsia::modular::session::AppConfig> default_component_args;
  bool has_component_args = data->has_component_args();
  xdr->FieldWithDefault(modular_config::kComponentArgs, data->mutable_component_args(),
                        XdrComponentArgs, has_component_args, std::move(default_component_args));

  std::vector<fuchsia::modular::session::AgentServiceIndexEntry> default_agent_service_index;
  bool has_agent_service_index = data->has_agent_service_index();
  xdr->FieldWithDefault(modular_config::kAgentServiceIndex, data->mutable_agent_service_index(),
                        XdrAgentServiceIndexEntry, has_agent_service_index,
                        std::move(default_agent_service_index));
}

}  // namespace modular
