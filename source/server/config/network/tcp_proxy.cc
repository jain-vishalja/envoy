#include "tcp_proxy.h"

#include "envoy/network/connection.h"
#include "envoy/server/instance.h"

#include "common/filter/tcp_proxy.h"

namespace Server {
namespace Configuration {

const std::string TcpProxyConfigFactory::TCP_PROXY_SCHEMA(R"EOF(
  {
      "$schema": "http://json-schema.org/schema#",
      "properties":{
        "stat_prefix" : {"type" : "string"},
        "cluster" : {"type" : "string"}
      },
      "required": ["stat_prefix", "cluster"],
      "additionalProperties": false
  }
  )EOF");

NetworkFilterFactoryCb TcpProxyConfigFactory::tryCreateFilterFactory(NetworkFilterType type,
                                                                     const std::string& name,
                                                                     const Json::Object& config,
                                                                     Server::Instance& server) {
  if (type != NetworkFilterType::Read || name != "tcp_proxy") {
    return nullptr;
  }

  config.validateSchema(TCP_PROXY_SCHEMA);

  Filter::TcpProxyConfigPtr filter_config(
      new Filter::TcpProxyConfig(config, server.clusterManager(), server.stats()));
  return [filter_config, &server](Network::FilterManager& filter_manager) -> void {
    filter_manager.addReadFilter(
        Network::ReadFilterPtr{new Filter::TcpProxy(filter_config, server.clusterManager())});
  };
}

/**
 * Static registration for the tcp_proxy filter. @see RegisterNetworkFilterConfigFactory.
 */
static RegisterNetworkFilterConfigFactory<TcpProxyConfigFactory> registered_;

} // Configuration
} // Server
