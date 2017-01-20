#include "configuration_impl.h"

#include "envoy/network/connection.h"
#include "envoy/runtime/runtime.h"
#include "envoy/server/instance.h"
#include "envoy/ssl/context_manager.h"

#include "common/common/utility.h"
#include "common/ratelimit/ratelimit_impl.h"
#include "common/ssl/context_config_impl.h"
#include "common/tracing/http_tracer_impl.h"
#include "common/upstream/cluster_manager_impl.h"

namespace Server {
namespace Configuration {

void FilterChainUtility::buildFilterChain(Network::FilterManager& filter_manager,
                                          const std::list<NetworkFilterFactoryCb>& factories) {
  for (const NetworkFilterFactoryCb& factory : factories) {
    factory(filter_manager);
  }

  filter_manager.initializeReadFilters();
}

MainImpl::MainImpl(Server::Instance& server) : server_(server) {}

void MainImpl::initialize(const Json::Object& json) {
  cluster_manager_factory_.reset(new Upstream::ProdClusterManagerFactory(
      server_.runtime(), server_.stats(), server_.threadLocal(), server_.random(),
      server_.dnsResolver(), server_.sslContextManager(), server_.dispatcher(),
      server_.localInfo()));
  cluster_manager_.reset(new Upstream::ClusterManagerImpl(
      *json.getObject("cluster_manager"), *cluster_manager_factory_, server_.stats(),
      server_.threadLocal(), server_.runtime(), server_.random(), server_.localInfo(),
      server_.accessLogManager()));

  std::vector<Json::ObjectPtr> listeners = json.getObjectArray("listeners");
  log().info("loading {} listener(s)", listeners.size());
  for (size_t i = 0; i < listeners.size(); i++) {
    log().info("listener #{}:", i);
    listeners_.emplace_back(
        Server::Configuration::ListenerPtr{new ListenerConfig(*this, *listeners[i])});
  }

  if (json.hasObject("statsd_local_udp_port")) {
    statsd_udp_port_.value(json.getInteger("statsd_local_udp_port"));
  }

  if (json.hasObject("statsd_tcp_cluster_name")) {
    statsd_tcp_cluster_name_.value(json.getString("statsd_tcp_cluster_name"));
  }

  stats_flush_interval_ =
      std::chrono::milliseconds(json.getInteger("stats_flush_interval_ms", 5000));

  if (json.hasObject("tracing")) {
    initializeTracers(*json.getObject("tracing"));
  } else {
    http_tracer_.reset(new Tracing::HttpNullTracer());
  }

  if (json.hasObject("rate_limit_service")) {
    Json::ObjectPtr rate_limit_service_config = json.getObject("rate_limit_service");
    std::string type = rate_limit_service_config->getString("type");
    if (type == "grpc_service") {
      ratelimit_client_factory_.reset(new RateLimit::GrpcFactoryImpl(
          *rate_limit_service_config->getObject("config"), *cluster_manager_));
    } else {
      throw EnvoyException(fmt::format("unknown rate limit service type '{}'", type));
    }
  } else {
    ratelimit_client_factory_.reset(new RateLimit::NullFactoryImpl());
  }
}

void MainImpl::initializeTracers(const Json::Object& tracing_configuration) {
  log().info("loading tracing configuration");

  // Initialize http sinks
  if (tracing_configuration.hasObject("http")) {
    http_tracer_.reset(new Tracing::HttpTracerImpl(server_.runtime(), server_.stats()));

    Json::ObjectPtr http_tracer_config = tracing_configuration.getObject("http");

    if (http_tracer_config->hasObject("sinks")) {
      std::vector<Json::ObjectPtr> sinks = http_tracer_config->getObjectArray("sinks");
      log().info(fmt::format("  loading {} http sink(s):", sinks.size()));

      for (const Json::ObjectPtr& sink : sinks) {
        std::string type = sink->getString("type");
        log().info(fmt::format("    loading {}", type));

        if (type == "lightstep") {
          ::Runtime::RandomGenerator& rand = server_.random();
          std::unique_ptr<lightstep::TracerOptions> opts(new lightstep::TracerOptions());
          opts->access_token = server_.api().fileReadToEnd(sink->getString("access_token_file"));
          StringUtil::rtrim(opts->access_token);

          opts->tracer_attributes["lightstep.component_name"] = server_.localInfo().clusterName();
          opts->guid_generator = [&rand]() { return rand.random(); };

          http_tracer_->addSink(Tracing::HttpSinkPtr{new Tracing::LightStepSink(
              *sink->getObject("config"), *cluster_manager_, server_.stats(), server_.localInfo(),
              server_.threadLocal(), server_.runtime(), std::move(opts))});
        } else {
          throw EnvoyException(fmt::format("unsupported sink type: '{}'", type));
        }
      }
    }
  } else {
    throw EnvoyException("incorrect tracing configuration");
  }
}

const std::list<Server::Configuration::ListenerPtr>& MainImpl::listeners() { return listeners_; }
const std::string MainImpl::ListenerConfig::LISTENER_SCHEMA(R"EOF(
  {
    "$schema": "http://json-schema.org/schema#",
    "definitions": {
      "ssl_context" : {
        "type" : "object",
        "properties" : {
          "cert_chain_file" : {"type" : "string"},
          "private_key_file": {"type" : "string"},
          "alpn_protocols" : {"type" : "string"},
          "alt_alpn_protocols": {"type" : "string"},
          "ca_cert_file" : {"type" : "string"},
          "verify_certificate_hash" : {"type" : "string"},
          "verify_subject_alt_name" : {"type" : "string"},
          "cipher_suites" : {"type" : "string"}
        },
        "required": ["cert_chain_file", "private_key_file"],
        "additionalProperties": false
      },
      "filters" : {
        "type" : "object",
        "properties" : {
          "type": {"type" : "string", "enum" :["read", "write", "both"]},
          "name" : {
            "type": "string",
            "enum" : ["client_ssl_auth", "echo", "mongo_proxy", "ratelimit", "tcp_proxy", "http_connection_manager"]
          },
          "config": {"type" : "object"}
        },
        "required": ["type", "name", "config"],
        "additionalProperties": false
      }
    },
    "properties": {
       "port": {"type": "number"},
       "filters" : {
         "type" : "array",
         "minItems" : 1,
         "items": {
           "type": "object",
           "properties": {"$ref" : "#/definitions/filters"}
         }
       },
       "ssl_context" : {"$ref" : "#/definitions/ssl_context"},
       "bind_to_port" : {"type": "boolean"},
       "use_proxy_proto" : {"type" : "boolean"},
       "use_original_dst" : {"type" : "boolean"}
    },
    "required": ["port", "filters"],
    "additionalProperties": false
  }
  )EOF");

MainImpl::ListenerConfig::ListenerConfig(MainImpl& parent, Json::Object& json)
    : parent_(parent), port_(json.getInteger("port")),
      scope_(parent_.server_.stats().createScope(fmt::format("listener.{}.", port_))) {
  log().info("  port={}", port_);

  json.validateSchema(LISTENER_SCHEMA);

  if (json.hasObject("ssl_context")) {
    Ssl::ContextConfigImpl context_config(*json.getObject("ssl_context"));
    ssl_context_ =
        parent_.server_.sslContextManager().createSslServerContext(*scope_, context_config);
  }

  bind_to_port_ = json.getBoolean("bind_to_port", true);
  use_proxy_proto_ = json.getBoolean("use_proxy_proto", false);
  use_original_dst_ = json.getBoolean("use_original_dst", false);

  std::vector<Json::ObjectPtr> filters = json.getObjectArray("filters");
  for (size_t i = 0; i < filters.size(); i++) {
    std::string string_type = filters[i]->getString("type");
    std::string string_name = filters[i]->getString("name");
    Json::ObjectPtr config = filters[i]->getObject("config");
    log().info("  filter #{}:", i);
    log().info("    type: {}", string_type);
    log().info("    name: {}", string_name);

    // Map filter type string to enum.
    NetworkFilterType type;
    if (string_type == "read") {
      type = NetworkFilterType::Read;
    } else if (string_type == "write") {
      type = NetworkFilterType::Write;
    } else if (string_type == "both") {
      type = NetworkFilterType::Both;
    } else {
      throw EnvoyException(fmt::format("invalid filter type '{}'", string_type));
    }

    // Now see if there is a factory that will accept the config.
    bool found_filter = false;
    for (NetworkFilterConfigFactory* config_factory : filterConfigFactories()) {
      NetworkFilterFactoryCb callback =
          config_factory->tryCreateFilterFactory(type, string_name, *config, parent_.server_);
      if (callback) {
        filter_factories_.push_back(callback);
        found_filter = true;
        break;
      }
    }

    if (!found_filter) {
      throw EnvoyException(
          fmt::format("unable to create filter factory for '{}'/'{}'", string_name, string_type));
    }
  }
}

void MainImpl::ListenerConfig::createFilterChain(Network::Connection& connection) {
  FilterChainUtility::buildFilterChain(connection, filter_factories_);
}

InitialImpl::InitialImpl(const Json::Object& json) {
  Json::ObjectPtr admin = json.getObject("admin");
  admin_.access_log_path_ = admin->getString("access_log_path");
  admin_.port_ = admin->getInteger("port");

  if (json.hasObject("flags_path")) {
    flags_path_.value(json.getString("flags_path"));
  }

  if (json.hasObject("runtime")) {
    runtime_.reset(new RuntimeImpl());
    runtime_->symlink_root_ = json.getObject("runtime")->getString("symlink_root");
    runtime_->subdirectory_ = json.getObject("runtime")->getString("subdirectory");
    runtime_->override_subdirectory_ =
        json.getObject("runtime")->getString("override_subdirectory", "");
  }
}

} // Configuration
} // Server
