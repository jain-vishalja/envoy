#include "envoy/upstream/upstream.h"

#include "common/ssl/context_manager_impl.h"
#include "common/stats/stats_impl.h"
#include "common/upstream/cluster_manager_impl.h"

#include "test/mocks/access_log/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/mocks.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::ReturnNew;
using testing::SaveArg;

namespace Upstream {

// The tests in this file are split between testing with real clusters and some with mock clusters.
// By default we setup to call the real cluster creation function. Individual tests can override
// the expectations when needed.
class TestClusterManagerFactory : public ClusterManagerFactory {
public:
  TestClusterManagerFactory() {
    ON_CALL(*this, clusterFromJson_(_, _, _, _))
        .WillByDefault(Invoke([&](const Json::Object& cluster, ClusterManager& cm,
                                  const Optional<SdsConfig>& sds_config,
                                  Outlier::EventLoggerPtr outlier_event_logger) -> Cluster* {
          return ClusterImplBase::create(cluster, cm, stats_, tls_, dns_resolver_,
                                         ssl_context_manager_, runtime_, random_, dispatcher_,
                                         sds_config, local_info_, outlier_event_logger).release();
        }));
  }

  Http::ConnectionPool::InstancePtr allocateConnPool(Event::Dispatcher&, ConstHostPtr host,
                                                     ResourcePriority) override {
    return Http::ConnectionPool::InstancePtr{allocateConnPool_(host)};
  }

  ClusterPtr clusterFromJson(const Json::Object& cluster, ClusterManager& cm,
                             const Optional<SdsConfig>& sds_config,
                             Outlier::EventLoggerPtr outlier_event_logger) override {
    return ClusterPtr{clusterFromJson_(cluster, cm, sds_config, outlier_event_logger)};
  }

  CdsApiPtr createCds(const Json::Object&, ClusterManager&) override {
    return CdsApiPtr{createCds_()};
  }

  MOCK_METHOD1(allocateConnPool_, Http::ConnectionPool::Instance*(ConstHostPtr host));
  MOCK_METHOD4(clusterFromJson_, Cluster*(const Json::Object& cluster, ClusterManager& cm,
                                          const Optional<SdsConfig>& sds_config,
                                          Outlier::EventLoggerPtr outlier_event_logger));
  MOCK_METHOD0(createCds_, CdsApi*());

  Stats::IsolatedStoreImpl stats_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Network::MockDnsResolver> dns_resolver_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Runtime::MockRandomGenerator> random_;
  Ssl::ContextManagerImpl ssl_context_manager_{runtime_};
  NiceMock<Event::MockDispatcher> dispatcher_;
  LocalInfo::MockLocalInfo local_info_;
};

class ClusterManagerImplTest : public testing::Test {
public:
  void create(const Json::Object& config) {
    cluster_manager_.reset(new ClusterManagerImpl(config, factory_, factory_.stats_, factory_.tls_,
                                                  factory_.runtime_, factory_.random_,
                                                  factory_.local_info_, log_manager_));
  }

  NiceMock<TestClusterManagerFactory> factory_;
  std::unique_ptr<ClusterManagerImpl> cluster_manager_;
  AccessLog::MockAccessLogManager log_manager_;
};

TEST_F(ClusterManagerImplTest, OutlierEventLog) {
  std::string json = R"EOF(
  {
    "outlier_detection": {
      "event_log_path": "foo"
    },
    "clusters": []
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  EXPECT_CALL(log_manager_, createAccessLog("foo"));
  create(*loader);
}

TEST_F(ClusterManagerImplTest, NoSdsConfig) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "sds",
      "lb_type": "round_robin"
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, UnknownClusterType) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "foo",
      "lb_type": "round_robin"
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, LocalClusterNotDefined) {
  std::string json = R"EOF(
  {
    "local_cluster_name": "new_cluster",
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}]
    },
    {
      "name": "cluster_2",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11002"}]
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, LocalClusterDefined) {
  std::string json = R"EOF(
  {
    "local_cluster_name": "new_cluster",
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}]
    },
    {
      "name": "cluster_2",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11002"}]
    },
    {
      "name": "new_cluster",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11002"}]
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  create(*loader);

  EXPECT_EQ(3UL, factory_.stats_.counter("cluster_manager.cluster_added").value());
  EXPECT_EQ(3UL, factory_.stats_.gauge("cluster_manager.total_clusters").value());
}

TEST_F(ClusterManagerImplTest, DuplicateCluster) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}]
    },
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}]
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, UnknownHcType) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}],
      "health_check": {
        "type": "foo"
      }
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, TcpHealthChecker) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}],
      "health_check": {
        "type": "tcp",
        "timeout_ms": 1000,
        "interval_ms": 1000,
        "unhealthy_threshold": 2,
        "healthy_threshold": 2,
        "send": [
          {"binary": "01"}
        ],
        "receive": [
          {"binary": "02"}
        ]
      }
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  Network::MockClientConnection* connection = new NiceMock<Network::MockClientConnection>();
  EXPECT_CALL(factory_.dispatcher_, createClientConnection_("tcp://127.0.0.1:11001"))
      .WillOnce(Return(connection));
  create(*loader);
}

TEST_F(ClusterManagerImplTest, UnknownCluster) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}]
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  create(*loader);
  EXPECT_EQ(nullptr, cluster_manager_->get("hello"));
  EXPECT_THROW(cluster_manager_->httpConnPoolForCluster("hello", ResourcePriority::Default),
               EnvoyException);
  EXPECT_THROW(cluster_manager_->tcpConnForCluster("hello"), EnvoyException);
  EXPECT_THROW(cluster_manager_->httpAsyncClientForCluster("hello"), EnvoyException);
}

TEST_F(ClusterManagerImplTest, ShutdownOrder) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}]
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  create(*loader);
  const Cluster& cluster = cluster_manager_->clusters().begin()->second;
  EXPECT_EQ("cluster_1", cluster.info()->name());

  // Local reference, primary reference, thread local reference, host reference.
  EXPECT_EQ(4U, cluster.info().use_count());

  // Thread local reference should be gone.
  factory_.tls_.shutdownThread();
  EXPECT_EQ(3U, cluster.info().use_count());
}

TEST_F(ClusterManagerImplTest, InitializeOrder) {
  std::string json = R"EOF(
  {
    "cds": {
      "cluster": {
        "fake": ""
      }
    },
    "clusters": [
    {
      "fake": ""
    },
    {
      "fake": ""
    }]
  }
  )EOF";

  MockCluster* cds_cluster = new NiceMock<MockCluster>();
  cds_cluster->info_->name_ = "cds_cluster";
  MockCluster* cluster1 = new NiceMock<MockCluster>();
  MockCluster* cluster2 = new NiceMock<MockCluster>();
  cluster2->info_->name_ = "fake_cluster2";

  InSequence s;
  EXPECT_CALL(factory_, clusterFromJson_(_, _, _, _)).WillOnce(Return(cds_cluster));
  ON_CALL(*cds_cluster, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cds_cluster, initialize());
  EXPECT_CALL(factory_, clusterFromJson_(_, _, _, _)).WillOnce(Return(cluster1));
  ON_CALL(*cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cluster1, initialize());
  EXPECT_CALL(factory_, clusterFromJson_(_, _, _, _)).WillOnce(Return(cluster2));
  ON_CALL(*cluster2, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));
  MockCdsApi* cds = new MockCdsApi();
  EXPECT_CALL(factory_, createCds_()).WillOnce(Return(cds));
  EXPECT_CALL(*cds, initialize());
  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  create(*loader);

  ReadyWatcher initialized;
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  EXPECT_CALL(*cluster2, initialize());
  cds_cluster->initialize_callback_();
  cluster1->initialize_callback_();

  EXPECT_CALL(initialized, ready());
  cluster2->initialize_callback_();
}

TEST_F(ClusterManagerImplTest, dynamicAddRemove) {
  std::string json = R"EOF(
  {
    "clusters": []
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  create(*loader);

  InSequence s;
  ReadyWatcher initialized;
  EXPECT_CALL(initialized, ready());
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  std::string json_api = R"EOF(
  {
    "name": "fake_cluster"
  }
  )EOF";

  Json::ObjectPtr loader_api = Json::Factory::LoadFromString(json_api);
  MockCluster* cluster1 = new NiceMock<MockCluster>();
  EXPECT_CALL(factory_, clusterFromJson_(_, _, _, _)).WillOnce(Return(cluster1));
  EXPECT_CALL(*cluster1, initializePhase()).Times(0);
  EXPECT_CALL(*cluster1, initialize());
  EXPECT_TRUE(cluster_manager_->addOrUpdatePrimaryCluster(*loader_api));

  EXPECT_EQ(cluster1->info_, cluster_manager_->get("fake_cluster"));
  EXPECT_EQ(1UL, factory_.stats_.gauge("cluster_manager.total_clusters").value());

  // Now try to update again but with the same hash (different white space).
  std::string json_api_2 = R"EOF(
  {
      "name":   "fake_cluster"
  }
  )EOF";

  loader_api = Json::Factory::LoadFromString(json_api_2);
  EXPECT_FALSE(cluster_manager_->addOrUpdatePrimaryCluster(*loader_api));

  // Now do it again with a different hash.
  std::string json_api_3 = R"EOF(
  {
      "name":   "fake_cluster",
      "blah": ""
  }
  )EOF";

  loader_api = Json::Factory::LoadFromString(json_api_3);
  MockCluster* cluster2 = new NiceMock<MockCluster>();
  cluster2->hosts_ = {HostPtr{new HostImpl(cluster2->info_, "tcp://127.0.0.1:80", false, 1, "")}};
  EXPECT_CALL(factory_, clusterFromJson_(_, _, _, _)).WillOnce(Return(cluster2));
  EXPECT_CALL(*cluster2, initializePhase()).Times(0);
  EXPECT_CALL(*cluster2, initialize());
  EXPECT_TRUE(cluster_manager_->addOrUpdatePrimaryCluster(*loader_api));

  EXPECT_EQ(cluster2->info_, cluster_manager_->get("fake_cluster"));
  EXPECT_EQ(1UL, cluster_manager_->clusters().size());
  Http::ConnectionPool::MockInstance* cp = new Http::ConnectionPool::MockInstance();
  EXPECT_CALL(factory_, allocateConnPool_(_)).WillOnce(Return(cp));
  EXPECT_EQ(cp,
            cluster_manager_->httpConnPoolForCluster("fake_cluster", ResourcePriority::Default));

  // Now remove it.
  EXPECT_TRUE(cluster_manager_->removePrimaryCluster("fake_cluster"));
  EXPECT_EQ(nullptr, cluster_manager_->get("fake_cluster"));
  EXPECT_EQ(0UL, cluster_manager_->clusters().size());

  // Remove an unknown cluster.
  EXPECT_FALSE(cluster_manager_->removePrimaryCluster("foo"));

  EXPECT_EQ(1UL, factory_.stats_.counter("cluster_manager.cluster_added").value());
  EXPECT_EQ(1UL, factory_.stats_.counter("cluster_manager.cluster_modified").value());
  EXPECT_EQ(1UL, factory_.stats_.counter("cluster_manager.cluster_removed").value());
  EXPECT_EQ(0UL, factory_.stats_.gauge("cluster_manager.total_clusters").value());
}

TEST_F(ClusterManagerImplTest, addOrUpdatePrimaryClusterStaticExists) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "fake": ""
    }]
  }
  )EOF";

  MockCluster* cluster1 = new NiceMock<MockCluster>();
  InSequence s;
  EXPECT_CALL(factory_, clusterFromJson_(_, _, _, _)).WillOnce(Return(cluster1));
  ON_CALL(*cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cluster1, initialize());

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  create(*loader);

  ReadyWatcher initialized;
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  EXPECT_CALL(initialized, ready());
  cluster1->initialize_callback_();

  std::string json_api = R"EOF(
  {
    "name": "fake_cluster"
  }
  )EOF";

  Json::ObjectPtr loader_api = Json::Factory::LoadFromString(json_api);
  EXPECT_FALSE(cluster_manager_->addOrUpdatePrimaryCluster(*loader_api));

  // Attempt to remove a static cluster.
  EXPECT_FALSE(cluster_manager_->removePrimaryCluster("fake_cluster"));
}

TEST_F(ClusterManagerImplTest, DynamicHostRemove) {
  std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "strict_dns",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://localhost:11001"}]
    }]
  }
  )EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);

  Network::DnsResolver::ResolveCb dns_callback;
  Event::MockTimer* dns_timer_ = new NiceMock<Event::MockTimer>(&factory_.dispatcher_);
  Network::MockActiveDnsQuery active_dns_query;
  EXPECT_CALL(factory_.dns_resolver_, resolve(_, _))
      .WillRepeatedly(DoAll(SaveArg<1>(&dns_callback), ReturnRef(active_dns_query)));
  create(*loader);

  // Test for no hosts returning the correct values before we have hosts.
  EXPECT_EQ(nullptr,
            cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::Default));
  EXPECT_EQ(nullptr, cluster_manager_->tcpConnForCluster("cluster_1").connection_);
  EXPECT_EQ(2UL, factory_.stats_.counter("cluster.cluster_1.upstream_cx_none_healthy").value());

  // Set up for an initialize callback.
  ReadyWatcher initialized;
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });
  EXPECT_CALL(initialized, ready());

  dns_callback({"127.0.0.1", "127.0.0.2"});

  // After we are initialized, we should immediately get called back if someone asks for an
  // initialize callback.
  EXPECT_CALL(initialized, ready());
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  EXPECT_CALL(factory_, allocateConnPool_(_))
      .Times(4)
      .WillRepeatedly(ReturnNew<Http::ConnectionPool::MockInstance>());

  // This should provide us a CP for each of the above hosts.
  Http::ConnectionPool::MockInstance* cp1 = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::Default));
  Http::ConnectionPool::MockInstance* cp2 = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::Default));
  Http::ConnectionPool::MockInstance* cp1_high = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::High));
  Http::ConnectionPool::MockInstance* cp2_high = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::High));

  EXPECT_NE(cp1, cp2);
  EXPECT_NE(cp1_high, cp2_high);
  EXPECT_NE(cp1, cp1_high);

  Http::ConnectionPool::Instance::DrainedCb drained_cb;
  EXPECT_CALL(*cp1, addDrainedCallback(_)).WillOnce(SaveArg<0>(&drained_cb));
  Http::ConnectionPool::Instance::DrainedCb drained_cb_high;
  EXPECT_CALL(*cp1_high, addDrainedCallback(_)).WillOnce(SaveArg<0>(&drained_cb_high));

  // Remove the first host, this should lead to the first cp being drained.
  dns_timer_->callback_();
  dns_callback({"127.0.0.2"});
  drained_cb();
  drained_cb = nullptr;
  EXPECT_CALL(factory_.tls_.dispatcher_, deferredDelete_(_)).Times(2);
  drained_cb_high();
  drained_cb_high = nullptr;

  // Make sure we get back the same connection pool for the 2nd host as we did before the change.
  Http::ConnectionPool::MockInstance* cp3 = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::Default));
  Http::ConnectionPool::MockInstance* cp3_high = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::High));
  EXPECT_EQ(cp2, cp3);
  EXPECT_EQ(cp2_high, cp3_high);

  // Now add and remove a host that we never have a conn pool to. This should not lead to any
  // drain callbacks, etc.
  dns_timer_->callback_();
  dns_callback({"127.0.0.2", "127.0.0.3"});
  dns_timer_->callback_();
  dns_callback({"127.0.0.2"});
}

} // Upstream
