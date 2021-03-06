# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# **************   NOTICE  *******************************************
# Facebook 2019 - Notice of Changes
# This file has been modified to extract only the Raft implementation
# out of Kudu into a fork known as kuduraft.
# ********************************************************************

#########################################
# consensus_metadata_proto
#########################################

PROTOBUF_GENERATE_CPP(
  METADATA_PROTO_SRCS METADATA_PROTO_HDRS METADATA_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES
    metadata.proto
    opid.proto
    replica_management.proto)
set(METADATA_PROTO_LIBS
  kudu_common_proto
  protobuf)
ADD_EXPORTABLE_LIBRARY(consensus_metadata_proto
  SRCS ${METADATA_PROTO_SRCS}
  DEPS ${METADATA_PROTO_LIBS}
  NONLINK_DEPS ${METADATA_PROTO_TGTS})

# NOTE: anirbanr-fb
# this was found to be not used in METADATA_PROTO_LIBS
# fs_proto

#########################################
# consensus_proto
#########################################

KRPC_GENERATE(
  CONSENSUS_KRPC_SRCS CONSENSUS_KRPC_HDRS CONSENSUS_KRPC_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES consensus.proto)
list(APPEND CONSENSUS_KRPC_SRCS opid_util.cc)
set(CONSENSUS_KRPC_LIBS
  consensus_metadata_proto
  krpc
  kudu_common_proto
  rpc_header_proto
  protobuf
  wire_protocol_proto)

# NOTE: anirbanr-fb
# cfile_proto
# tablet_proto
# tserver_admin_proto
ADD_EXPORTABLE_LIBRARY(consensus_proto
  SRCS ${CONSENSUS_KRPC_SRCS}
  DEPS ${CONSENSUS_KRPC_LIBS}
  NONLINK_DEPS ${CONSENSUS_KRPC_TGTS})

#########################################
# persistent_vars_proto
#########################################

PROTOBUF_GENERATE_CPP(
  PERSISTENT_VARS_PROTO_SRCS PERSISTENT_VARS_PROTO_HDRS PERSISTENT_VARS_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES
    persistent_vars.proto)
set(PERSISTENT_VARS_PROTO_LIBS
  kudu_common_proto
  protobuf)
ADD_EXPORTABLE_LIBRARY(persistent_vars_proto
  SRCS ${PERSISTENT_VARS_PROTO_SRCS}
  DEPS ${PERSISTENT_VARS_PROTO_LIBS}
  NONLINK_DEPS ${PERSISTENT_VARS_PROTO_TGTS})

#########################################
# log_proto
#########################################

PROTOBUF_GENERATE_CPP(
  LOG_PROTO_SRCS LOG_PROTO_HDRS LOG_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES log.proto)

add_library(log_proto ${LOG_PROTO_SRCS} ${LOG_PROTO_HDRS})
target_link_libraries(log_proto
  consensus_proto)

set(LOG_SRCS
  log_util.cc
  log.cc
  log_anchor_registry.cc
  log_index.cc
  log_reader.cc
  log_metrics.cc
)

add_library(log ${LOG_SRCS})
target_link_libraries(log
  clock
  gutil
  kudu_common
  kudu_fs
  kudu_util_compression
  consensus_proto
  log_proto
  consensus_metadata_proto
  persistent_vars_proto)

set(CONSENSUS_SRCS
  consensus_meta.cc
  consensus_meta_manager.cc
  consensus_peers.cc
  consensus_queue.cc
  leader_election.cc
  log_cache.cc
  peer_manager.cc
  persistent_vars.cc
  persistent_vars_manager.cc
  pending_rounds.cc
  quorum_util.cc
  raft_consensus.cc
  routing.cc
  time_manager.cc
)

add_library(consensus ${CONSENSUS_SRCS})
target_link_libraries(consensus
  consensus_proto
  kudu_common
  log
  protobuf)

SET_KUDU_TEST_LINK_LIBS(
log
consensus
kudu_util
)
# cfile

#SET_KUDU_TEST_LINK_LIBS(
#log
#consensus
#tserver_proto
#cfile
#tablet
#kudu_util)

#ADD_KUDU_TEST(log-test)
ADD_KUDU_TEST(time_manager-test)
ADD_KUDU_TEST(leader_election-test)
ADD_KUDU_TEST(log_index-test)
ADD_KUDU_TEST(quorum_util-test)
ADD_KUDU_TEST(consensus_meta-test)
ADD_KUDU_TEST(log_anchor_registry-test)
ADD_KUDU_TEST(consensus_meta_manager-test)
ADD_KUDU_TEST(consensus_meta_manager-stress-test RUN_SERIAL true)
ADD_KUDU_TEST(raft_consensus_quorum-test)
#ADD_KUDU_TEST(consensus_queue-test)

ADD_KUDU_TEST(consensus_peers-test)
#ADD_KUDU_TEST(log_cache-test PROCESSORS 2)
#ADD_KUDU_TEST(mt-log-test PROCESSORS 5)
ADD_KUDU_TEST(routing-test)

# Our current version of gmock overrides virtual functions without adding
# the 'override' keyword which, since our move to c++11, make the compiler
# issue warnings. This suppresses those warnings.
# TODO This problem has been resolved in recent versions of gtest/gmock (now
# googletest). Remove this when we upgrade.
#set_source_files_properties(raft_consensus-test.cc
#    PROPERTIES COMPILE_FLAGS -Wno-inconsistent-missing-override)
