// Copyright (c) 2015, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include <boost/foreach.hpp>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>
#include <tr1/memory>
#include <tr1/unordered_map>
#include <string>

#include "kudu/client/client.h"
#include "kudu/client/client-test-util.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/common/wire_protocol-test-util.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/master.proxy.h"
#include "kudu/integration-tests/cluster_itest_util.h"
#include "kudu/integration-tests/external_mini_cluster.h"
#include "kudu/integration-tests/test_workload.h"
#include "kudu/tablet/tablet.pb.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/rpc/messenger.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/pstack_watcher.h"
#include "kudu/util/test_util.h"

using kudu::client::KuduClient;
using kudu::client::KuduClientBuilder;
using kudu::client::KuduTableCreator;
using kudu::itest::TabletServerMap;
using kudu::itest::TServerDetails;
using kudu::master::MasterServiceProxy;
using kudu::rpc::Messenger;
using kudu::rpc::MessengerBuilder;
using kudu::tablet::TABLET_DATA_DELETED;
using kudu::tablet::TABLET_DATA_TOMBSTONED;
using kudu::tablet::TabletSuperBlockPB;
using kudu::tserver::ListTabletsResponsePB;
using std::numeric_limits;
using std::string;
using std::tr1::shared_ptr;
using std::tr1::unordered_map;
using std::vector;
using strings::Substitute;

namespace kudu {

class DeleteTableTest : public KuduTest {
 public:
  virtual void SetUp() OVERRIDE {
    KuduTest::SetUp();

    ExternalMiniClusterOptions opts;
    opts.num_tablet_servers = 3;
    cluster_.reset(new ExternalMiniCluster(opts));
    ASSERT_OK(cluster_->Start());
    KuduClientBuilder builder;
    ASSERT_OK(cluster_->CreateClient(builder, &client_));
  }

  virtual void TearDown() OVERRIDE {
    if (HasFatalFailure()) {
      for (int i = 0; i < 3; i++) {
        if (!cluster_->tablet_server(i)->IsProcessAlive()) {
          LOG(INFO) << "Tablet server " << i << " is not running. Cannot dump its stacks.";
          continue;
        }
        WARN_NOT_OK(PstackWatcher::DumpPidStacks(cluster_->tablet_server(i)->pid()),
                    "Couldn't dump stacks");
      }
    }
    cluster_->Shutdown();
    KuduTest::TearDown();
  }

 protected:
  Status ListFilesInDir(const string& path, vector<string>* entries);
  int CountFilesInDir(const string& path);
  int CountWALSegmentsOnTS(int index);
  int CountWALSegmentsForTabletOnTS(int index, const string& tablet_id);
  bool DoesConsensusMetaExistForTabletOnTS(int index, const string& tablet_id);

  int CountReplicasInMetadataDirs();
  Status CheckNoDataOnTS(int index);
  Status CheckNoData();

  Status ReadTabletSuperBlockOnTS(int index, const string& tablet_id,
                                  TabletSuperBlockPB* sb);
  Status CheckTabletTombstonedOnTS(int index, const string& tablet_id);

  void WaitForNoData();
  void WaitForNoDataOnTS(int index);
  void WaitForWALSegmentsForTabletOnTS(int index, const string& tablet_id);
  void WaitForTabletTombstonedOnTS(int index, const string& tablet_id);

  void WaitForReplicaCount(int expected);
  void WaitForTSToCrash(int index);
  void WaitForAllTSToCrash();

  // Delete the given table. If the operation times out, dumps the master stacks
  // to help debug master-side deadlocks.
  void DeleteTable(const string& table_name);

  // Repeatedly try to delete the tablet, retrying on failure up to the
  // specified timeout. Deletion can fail when other operations, such as
  // bootstrap, are running.
  void DeleteTabletWithRetries(const TServerDetails* ts, const string& tablet_id,
                               tablet::TabletDataState delete_type, const MonoDelta& timeout);

  gscoped_ptr<ExternalMiniCluster> cluster_;
  shared_ptr<KuduClient> client_;
};

Status DeleteTableTest::ListFilesInDir(const string& path, vector<string>* entries) {
  RETURN_NOT_OK(env_->GetChildren(path, entries));
  vector<string>::iterator iter = entries->begin();
  while (iter != entries->end()) {
    if (*iter == "." || *iter == "..") {
      iter = entries->erase(iter);
      continue;
    }
    ++iter;
  }
  return Status::OK();
}

int DeleteTableTest::CountFilesInDir(const string& path) {
  vector<string> entries;
  Status s = ListFilesInDir(path, &entries);
  if (!s.ok()) return 0;
  return entries.size();
}

int DeleteTableTest::CountWALSegmentsOnTS(int index) {
  string data_dir = cluster_->tablet_server(index)->data_dir();
  string ts_wal_dir = JoinPathSegments(data_dir, FsManager::kWalDirName);
  vector<string> tablets;
  CHECK_OK(ListFilesInDir(ts_wal_dir, &tablets));
  int total_segments = 0;
  BOOST_FOREACH(const string& tablet, tablets) {
    string tablet_wal_dir = JoinPathSegments(ts_wal_dir, tablet);
    total_segments += CountFilesInDir(tablet_wal_dir);
  }
  return total_segments;
}

int DeleteTableTest::CountWALSegmentsForTabletOnTS(int index, const string& tablet_id) {
  string data_dir = cluster_->tablet_server(index)->data_dir();
  string wal_dir =
      JoinPathSegments(JoinPathSegments(data_dir, FsManager::kWalDirName), tablet_id);
  if (!env_->FileExists(wal_dir)) {
    return 0;
  }
  return CountFilesInDir(wal_dir);
}

bool DeleteTableTest::DoesConsensusMetaExistForTabletOnTS(int index, const string& tablet_id) {
  string data_dir = cluster_->tablet_server(index)->data_dir();
  string path = JoinPathSegments(
      JoinPathSegments(data_dir, FsManager::kConsensusMetadataDirName), tablet_id);
  return env_->FileExists(path);
}

int DeleteTableTest::CountReplicasInMetadataDirs() {
  // Rather than using FsManager's functionality for listing blocks, we just manually
  // list the contents of the metadata directory. This is because we're using an
  // external minicluster, and initializing a new FsManager to point at the running
  // tablet servers isn't easy.
  int count = 0;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    string data_dir = cluster_->tablet_server(i)->data_dir();
    count += CountFilesInDir(JoinPathSegments(data_dir, FsManager::kTabletMetadataDirName));
  }
  return count;
}

Status DeleteTableTest::CheckNoDataOnTS(int index) {
  string data_dir = cluster_->tablet_server(index)->data_dir();
  if (CountFilesInDir(JoinPathSegments(data_dir, FsManager::kTabletMetadataDirName)) > 0) {
    return Status::IllegalState("tablet metadata blocks still exist", data_dir);
  }
  if (CountWALSegmentsOnTS(index) > 0) {
    return Status::IllegalState("wals still exist", data_dir);
  }
  if (CountFilesInDir(JoinPathSegments(data_dir, FsManager::kConsensusMetadataDirName)) > 0) {
    return Status::IllegalState("consensus metadata still exists", data_dir);
  }
  return Status::OK();;
}

Status DeleteTableTest::CheckNoData() {
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    RETURN_NOT_OK(CheckNoDataOnTS(i));
  }
  return Status::OK();;
}

Status DeleteTableTest::ReadTabletSuperBlockOnTS(int index,
                                                 const string& tablet_id,
                                                 TabletSuperBlockPB* sb) {
  string data_dir = cluster_->tablet_server(index)->data_dir();
  string meta_dir = JoinPathSegments(data_dir, FsManager::kTabletMetadataDirName);
  string superblock_path = JoinPathSegments(meta_dir, tablet_id);
  return pb_util::ReadPBContainerFromPath(env_.get(), superblock_path, sb);
}

Status DeleteTableTest::CheckTabletTombstonedOnTS(int index, const string& tablet_id) {
  // We simply check that no WALs exist and that the superblock indicates
  // TOMBSTONED.
  TabletSuperBlockPB sb;
  RETURN_NOT_OK(ReadTabletSuperBlockOnTS(index, tablet_id, &sb));
  if (sb.tablet_data_state() != TABLET_DATA_TOMBSTONED) {
    return Status::IllegalState("Tablet metadata not not TOMBSTONED",
                                TabletDataState_Name(sb.tablet_data_state()));
  }
  if (CountWALSegmentsForTabletOnTS(index, tablet_id) > 0) {
    return Status::IllegalState("WAL segments exist for tablet", tablet_id);
  }
  if (!DeleteTableTest::DoesConsensusMetaExistForTabletOnTS(index, tablet_id)) {
    return Status::IllegalState("Expected cmeta for tablet " + tablet_id + " but it doesn't exist");
  }
  return Status::OK();
}

void DeleteTableTest::WaitForNoData() {
  Status s;
  for (int i = 0; i < 1000; i++) {
    s = CheckNoData();
    if (s.ok()) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(s);
}

void DeleteTableTest::WaitForNoDataOnTS(int index) {
  Status s;
  for (int i = 0; i < 1000; i++) {
    s = CheckNoDataOnTS(index);
    if (s.ok()) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(s);
}

void DeleteTableTest::WaitForWALSegmentsForTabletOnTS(int index, const string& tablet_id) {
  for (int i = 0; i < 3000; i++) {
    if (CountWALSegmentsForTabletOnTS(index, tablet_id) > 0) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  FAIL() << "No WALs appeared for TS " << index << " in tablet " << tablet_id;
}

void DeleteTableTest::WaitForTabletTombstonedOnTS(int index, const string& tablet_id) {
  Status s;
  for (int i = 0; i < 3000; i++) {
    s = CheckTabletTombstonedOnTS(index, tablet_id);
    if (s.ok()) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(s);
}

void DeleteTableTest::WaitForReplicaCount(int expected) {
  Status s;
  for (int i = 0; i < 1000; i++) {
    if (CountReplicasInMetadataDirs() == expected) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_EQ(expected, CountReplicasInMetadataDirs());
}

void DeleteTableTest::DeleteTable(const string& table_name) {
  Status s = client_->DeleteTable(table_name);
  if (s.IsTimedOut()) {
    WARN_NOT_OK(PstackWatcher::DumpPidStacks(cluster_->master()->pid()),
                        "Couldn't dump stacks");
  }
  ASSERT_OK(s);
}

void DeleteTableTest::WaitForTSToCrash(int index) {
  ExternalTabletServer* ts = cluster_->tablet_server(index);
  for (int i = 0; i < 1000; i++) {
    if (!ts->IsProcessAlive()) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  FAIL() << "TS " << ts->instance_id().permanent_uuid() << " did not crash!";
}

void DeleteTableTest::WaitForAllTSToCrash() {
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    NO_FATALS(WaitForTSToCrash(i));
  }
}

void DeleteTableTest::DeleteTabletWithRetries(const TServerDetails* ts,
                                              const string& tablet_id,
                                              tablet::TabletDataState delete_type,
                                              const MonoDelta& timeout) {
  MonoTime start(MonoTime::Now(MonoTime::FINE));
  MonoTime deadline = start;
  deadline.AddDelta(timeout);
  Status s;
  while (true) {
    s = itest::DeleteTablet(ts, tablet_id, delete_type, timeout);
    if (s.ok()) return;
    if (deadline.ComesBefore(MonoTime::Now(MonoTime::FINE))) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(s);
}

TEST_F(DeleteTableTest, TestDeleteEmptyTable) {
  // Create a table on the cluster. We're just using TestWorkload
  // as a convenient way to create it.
  TestWorkload(cluster_.get()).Setup();

  // The table should have replicas on all three tservers.
  NO_FATALS(WaitForReplicaCount(3));

  // Delete it and wait for the replicas to get deleted.
  NO_FATALS(DeleteTable(TestWorkload::kDefaultTableName));
  NO_FATALS(WaitForNoData());
}

TEST_F(DeleteTableTest, TestDeleteTableWithConcurrentWrites) {
  int n_iters = AllowSlowTests() ? 20 : 1;
  for (int i = 0; i < n_iters; i++) {
    TestWorkload workload(cluster_.get());
    workload.set_table_name(Substitute("table-$0", i));

    // We'll delete the table underneath the writers, so we expcted
    // a NotFound error during the writes.
    workload.set_not_found_allowed(true);
    workload.Setup();

    // Start the workload, and wait to see some rows actually inserted
    workload.Start();
    while (workload.rows_inserted() < 100) {
      SleepFor(MonoDelta::FromMilliseconds(10));
    }

    // Delete it and wait for the replicas to get deleted.
    NO_FATALS(DeleteTable(workload.table_name()));
    NO_FATALS(WaitForNoData());

    // Sleep just a little longer to make sure client threads send
    // requests to the missing tablets.
    SleepFor(MonoDelta::FromMilliseconds(50));

    workload.StopAndJoin();
    cluster_->AssertNoCrashes();
  }
}

// Parameterized test case for TABLET_DATA_DELETED deletions.
class DeleteTableDeletedParamTest : public DeleteTableTest,
                                    public ::testing::WithParamInterface<const char*> {
};

// Test that if a server crashes mid-delete that the delete will be rolled
// forward on startup. Parameterized by different fault flags that cause a
// crash at various points.
TEST_P(DeleteTableDeletedParamTest, TestRollForwardDelete) {
  const string fault_flag = GetParam();
  LOG(INFO) << "Running with fault flag: " << fault_flag;

  // Dynamically set the fault flag so they crash when DeleteTablet() is called
  // by the Master.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(i), fault_flag, "1.0"));
  }

  // Create a table on the cluster. We're just using TestWorkload
  // as a convenient way to create it.
  TestWorkload(cluster_.get()).Setup();

  // The table should have replicas on all three tservers.
  NO_FATALS(WaitForReplicaCount(3));

  // Delete it and wait for the tablet servers to crash.
  NO_FATALS(DeleteTable(TestWorkload::kDefaultTableName));
  NO_FATALS(WaitForAllTSToCrash());

  // There should still be data left on disk.
  Status s = CheckNoData();
  ASSERT_TRUE(s.IsIllegalState()) << s.ToString();

  // Now restart the tablet servers. They should roll forward their deletes.
  // We don't have to reset the fault flag here because it was set dynamically.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    cluster_->tablet_server(i)->Shutdown();
    ASSERT_OK(cluster_->tablet_server(i)->Restart());
  }
  NO_FATALS(WaitForNoData());
}

// Faults appropriate for the TABLET_DATA_DELETED case.
const char* deleted_faults[] = {"fault_crash_after_blocks_deleted",
                                "fault_crash_after_wal_deleted",
                                "fault_crash_after_cmeta_deleted"};

INSTANTIATE_TEST_CASE_P(FaultFlags, DeleteTableDeletedParamTest,
                        ::testing::ValuesIn(deleted_faults));

// Parameterized test case for TABLET_DATA_TOMBSTONED deletions.
class DeleteTableTombstonedParamTest : public DeleteTableTest,
                                       public ::testing::WithParamInterface<const char*> {
};

// Regression test for tablet tombstoning. Tests:
// 1. basic creation & tombstoning of a tablet.
// 2. roll-forward (crash recovery) of a partially-completed tombstoning of a tablet.
// 3. permanent deletion of a TOMBSTONED tablet
//    (transition from TABLET_DATA_TOMBSTONED to TABLET_DATA_DELETED).
TEST_P(DeleteTableTombstonedParamTest, TestTabletTombstone) {
  const string fault_flag = GetParam();
  LOG(INFO) << "Running with fault flag: " << fault_flag;

  MonoDelta timeout = MonoDelta::FromSeconds(30);

  // Create a table with 2 tablets. We delete the first tablet without
  // injecting any faults, then we delete the second tablet while exercising
  // several fault injection points.
  const int kNumTablets = 2;
  vector<string> split_keys;
  Schema schema(GetSimpleTestSchema());
  client::KuduSchema client_schema(client::KuduSchemaFromSchema(schema));
  gscoped_ptr<KuduPartialRow> split_key(client_schema.NewRow());
  ASSERT_OK(split_key->SetInt32(0, numeric_limits<int32_t>::max() / kNumTablets));
  split_keys.push_back(split_key->ToEncodedRowKeyOrDie());
  gscoped_ptr<KuduTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_OK(table_creator->table_name(TestWorkload::kDefaultTableName)
                          .split_keys(split_keys)
                          .schema(&client_schema)
                          .num_replicas(3)
                          .Create());

  // Start a workload on the cluster, and run it until we find WALs on disk.
  TestWorkload workload(cluster_.get());
  workload.Setup();

  // The table should have 2 tablets (1 split) on all 3 tservers (for a total of 6).
  NO_FATALS(WaitForReplicaCount(6));

  // Set up the proxies so we can easily send DeleteTablet() RPCs.
  unordered_map<string, TServerDetails*> ts_map;
  ASSERT_OK(itest::CreateTabletServerMap(cluster_->master_proxy().get(),
                                         cluster_->messenger(),
                                         &ts_map));
  TServerDetails* ts = ts_map[cluster_->tablet_server(0)->instance_id().permanent_uuid()];

  // Ensure the tablet server is reporting 2 tablets.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(itest::ListTablets(ts, timeout, &tablets));
  ASSERT_EQ(2, tablets.size());

  // Run the workload against whoever the leader is until WALs appear on TS 0
  // for the tablets we created.
  const int kTsIndex = 0; // Index of the tablet server we'll use for the test.
  workload.Start();
  NO_FATALS(WaitForWALSegmentsForTabletOnTS(kTsIndex, tablets[0].tablet_status().tablet_id()));
  NO_FATALS(WaitForWALSegmentsForTabletOnTS(kTsIndex, tablets[1].tablet_status().tablet_id()));
  workload.StopAndJoin();

  // Shut down the master and the other tablet servers so they don't interfere
  // by attempting to create tablets or remote bootstrap while we delete tablets.
  cluster_->master()->Shutdown();
  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();

  // Tombstone the first tablet.
  string tablet_id = tablets[0].tablet_status().tablet_id();
  LOG(INFO) << "Tombstoning first tablet " << tablet_id << "...";
  ASSERT_TRUE(DoesConsensusMetaExistForTabletOnTS(kTsIndex, tablet_id)) << tablet_id;
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, timeout));
  LOG(INFO) << "Waiting for first tablet to be tombstoned...";
  NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id));

  // Now tombstone the 2nd tablet, causing a fault.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(kTsIndex), fault_flag, "1.0"));
  tablet_id = tablets[1].tablet_status().tablet_id();
  LOG(INFO) << "Tombstoning second tablet " << tablet_id << "...";
  ignore_result(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, timeout));
  NO_FATALS(WaitForTSToCrash(kTsIndex));

  // Restart the tablet server and wait for the WALs to be deleted and for the
  // superblock to show that it is tombstoned.
  cluster_->tablet_server(kTsIndex)->Shutdown();
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart());
  LOG(INFO) << "Waiting for second tablet to be tombstoned...";
  NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id));

  // The tombstoned tablets will still show up in ListTablets(),
  // just with their data state set as TOMBSTONED.
  ASSERT_OK(itest::ListTablets(ts, timeout, &tablets));
  ASSERT_EQ(2, tablets.size());
  BOOST_FOREACH(const ListTabletsResponsePB::StatusAndSchemaPB& t, tablets) {
    ASSERT_EQ(TABLET_DATA_TOMBSTONED, t.tablet_status().tablet_data_state())
        << t.tablet_status().tablet_id() << " not tombstoned";
  }

  // Finally, delete all tablets on the TS, and wait for all data to be gone.
  LOG(INFO) << "Deleting all tablets...";
  BOOST_FOREACH(const ListTabletsResponsePB::StatusAndSchemaPB& tablet, tablets) {
    string tablet_id = tablet.tablet_status().tablet_id();
    // We need retries here, since some of the tablets may still be
    // bootstrapping after being restarted above.
    NO_FATALS(DeleteTabletWithRetries(ts, tablet_id, TABLET_DATA_DELETED, timeout));
  }
  NO_FATALS(WaitForNoDataOnTS(kTsIndex));
  STLDeleteValues(&ts_map);
}

// Faults appropriate for the TABLET_DATA_TOMBSTONED case.
// Tombstoning a tablet does not delete the consensus metadata.
const char* tombstoned_faults[] = {"fault_crash_after_blocks_deleted",
                                   "fault_crash_after_wal_deleted"};

INSTANTIATE_TEST_CASE_P(FaultFlags, DeleteTableTombstonedParamTest,
                        ::testing::ValuesIn(tombstoned_faults));

} // namespace kudu
