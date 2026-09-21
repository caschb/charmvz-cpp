// Stage 2 pairing state and Stage 3 reconstruction, asserted against the
// Parquet files the pipeline writes: incomplete intervals survive with NULL
// ends, messages link on (src_pe, event) per destination, the clock reference
// gates every cross-PE derivation, and the thirteen schemas stay frozen.
//
// Timestamp semantics come from charm/src/ck-perf/trace-projections.C, not the
// format notes: a CREATION's last field is the interval creationDone() stored
// (`curTime - log.time`, on the source PE's clock) and a BEGIN_PROCESSING's
// irecvtime is written as -1 only when the runtime had no value, otherwise as
// the constant 0.0 that beginExecuteLocal() passes.

#include "log_parser.h"
#include "metadata_tables.h"
#include "rc_parser.h"
#include "reconstruction.h"
#include "schema.h"
#include "sts_parser.h"
#include "trace_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

namespace {

using charmvz::test::ParquetTable;
using charmvz::test::TempTrace;

constexpr auto kSts = "PROJECTIONS_ID \n"
                      "VERSION 11.0\n"
                      "PROCESSORS 3\n"
                      "TOTAL_CHARES 2\n"
                      "CHARE 0 \"AGroup\" -1\n"
                      "CHARE 1 \"Array1D\" 1\n"
                      "ENTRY CHARE 0 \"dummy_thread_ep\" 1 0\n"
                      "ENTRY CHARE 10 \"gmethod()\" 0 0\n"
                      "ENTRY CHARE 11 \"one(dummyMsg*)\" 1 0\n"
                      "MESSAGE 0 64\n"
                      "TOTAL_EVENTS 0\n"
                      "TOTAL_STATS 0\n"
                      "END\n";

// The whole pipeline, with the clock reference read from the fixture's
// .projrc (absent unless the test wrote one).
auto run_all(const TempTrace &trace) -> charmvz::LogParserResult {
  const auto sts = charmvz::parse_sts_file(trace.sts_path());
  const auto rc = charmvz::parse_rc_file(trace.rc_path());
  charmvz::write_metadata_tables(sts, trace.out_dir());
  auto result = charmvz::process_logs(trace.log_paths(), sts, rc,
                                      trace.out_dir(), charmvz::NO_STEP_EVENT);
  charmvz::reconstruct_message_and_migration(result, rc, trace.out_dir());
  charmvz::reconstruct_simulation_steps(result, trace.out_dir());
  return result;
}

auto s(int64_t v) -> std::string { return std::to_string(v); }

// CREATION: type mIdx eIdx itime event pe msglen irecvtime
auto creation(int event, int64_t itime, int64_t delta, int src_pe)
    -> std::string {
  return "1 0 11 " + s(itime) + " " + s(event) + " " + s(src_pe) + " 64 " +
         s(delta) + "\n";
}
auto bcast(int event, int64_t itime, int64_t delta, int src_pe, int fanout)
    -> std::string {
  return "20 0 10 " + s(itime) + " " + s(event) + " " + s(src_pe) + " 64 " +
         s(delta) + " " + s(fanout) + "\n";
}
auto mcast(int event, int64_t itime, int64_t delta, int src_pe,
           const std::string &pes, int count) -> std::string {
  return "21 0 11 " + s(itime) + " " + s(event) + " " + s(src_pe) + " 64 " +
         s(delta) + " " + s(count) + " " + pes + "\n";
}
// BEGIN_PROCESSING for the 1-D array: type mIdx eIdx itime event pe msglen
// irecvtime idx icputime. `recv` is the literal token so a test can write
// the runtime's -1 encoding.
auto begin(int event, int64_t itime, int src_pe, int index,
           const std::string &recv = "0", int64_t cpu = 0) -> std::string {
  return "2 0 11 " + s(itime) + " " + s(event) + " " + s(src_pe) + " 64 " +
         recv + " " + s(index) + " " + s(cpu) + "\n";
}
auto end(int event, int64_t itime, int src_pe, int64_t cpu = 0) -> std::string {
  return "3 0 11 " + s(itime) + " " + s(event) + " " + s(src_pe) + " 0 " +
         s(cpu) + "\n";
}

constexpr auto kUnavailable = "18446744073709551615";

struct MessageRow {
  int64_t src_pe;
  int64_t event;
  int64_t send;
  int64_t enqueue;
  bool is_broadcast;
  std::optional<int64_t> fanout;
  std::optional<int64_t> dst;
  std::optional<int64_t> recv;
  std::optional<int64_t> exec;
  std::optional<int64_t> s2e;
  std::optional<int64_t> e2e;
  std::optional<int64_t> end2end;
};

auto read_messages(const std::string &out_dir) -> std::vector<MessageRow> {
  ParquetTable t(out_dir + "/message.parquet");
  const auto src = t.ints("src_pe");
  const auto evt = t.ints("event");
  const auto send = t.ints("send_time_us");
  const auto enq = t.ints("enqueue_time_us");
  const auto bc = t.bools("is_broadcast");
  const auto fan = t.ints("broadcast_fanout");
  const auto dst = t.ints("dst_pe");
  const auto recv = t.ints("recv_time_us");
  const auto exec = t.ints("exec_start_time_us");
  const auto s2e = t.ints("send_to_enqueue_us");
  const auto e2e = t.ints("enqueue_to_exec_us");
  const auto end2end = t.ints("end_to_end_us");
  std::vector<MessageRow> rows;
  for (size_t i = 0; i < src.size(); ++i) {
    rows.push_back({*src[i], *evt[i], *send[i], *enq[i], *bc[i], fan[i], dst[i],
                    recv[i], exec[i], s2e[i], e2e[i], end2end[i]});
  }
  return rows;
}

} // namespace

TEST_CASE("A BEGIN_PROCESSING with no END survives the end of the log",
          "[log_parser][execution][incomplete]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, begin(1, 1000, 0, 7, "0", 900) + end(1, 1200, 0, 1050) +
                       begin(2, 1300, 0, 7, "0", 1100));
  const auto result = run_all(trace);

  CHECK(result.diagnostics.incomplete_executions == 1);
  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 2);
  const auto ev = execs.ints("event");
  const auto end_t = execs.ints("end_time_us");
  const auto end_cpu = execs.ints("end_cpu_us");
  const auto wall = execs.ints("wall_duration_us");
  const auto cpu = execs.ints("cpu_duration_us");
  const auto inst = execs.ints("instance_id");
  const auto start_cpu = execs.ints("start_cpu_us");
  // The complete one keeps its arithmetic.
  CHECK(ev[0] == 1);
  CHECK(end_t[0] == 1200);
  CHECK(wall[0] == 200);
  CHECK(cpu[0] == 150);
  // The truncated one keeps every begin-side field and nothing end-derived.
  CHECK(ev[1] == 2);
  CHECK(execs.ints("start_time_us")[1] == 1300);
  CHECK(start_cpu[1] == 1100);
  CHECK(inst[1] == inst[0]);
  CHECK_FALSE(end_t[1].has_value());
  CHECK_FALSE(end_cpu[1].has_value());
  CHECK_FALSE(wall[1].has_value());
  CHECK_FALSE(cpu[1].has_value());
}

TEST_CASE("An END_PROCESSING with no BEGIN is diagnosed and dropped",
          "[log_parser][execution][incomplete]") {
  // The start time and index tuple exist only on the BEGIN record, so there
  // is nothing to write without inventing them.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, end(9, 1200, 0));
  const auto result = run_all(trace);

  CHECK(result.diagnostics.orphan_end_processing == 1);
  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  CHECK(execs.rows() == 0);
}

TEST_CASE("A repeated open serial writes both observations",
          "[log_parser][execution][regression]") {
  // Two BEGINs on one serial with no END between them. The earlier one must
  // not vanish under the later one; it is written as incomplete.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0,
                begin(1, 1000, 0, 7) + begin(1, 1500, 0, 8) + end(1, 1700, 0));
  const auto result = run_all(trace);

  CHECK(result.diagnostics.duplicate_open_executions == 1);
  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 2);
  CHECK(execs.ints("start_time_us")[0] == 1000);
  CHECK_FALSE(execs.ints("end_time_us")[0].has_value());
  CHECK(execs.ints("start_time_us")[1] == 1500);
  CHECK(execs.ints("end_time_us")[1] == 1700);
}

TEST_CASE("Executions pair on source PE and serial, not serial alone",
          "[log_parser][execution][regression]") {
  // Two sources whose serials coincide overlap on one PE. END_PROCESSING
  // repeats execPe and execEvent, so each END closes only its own BEGIN.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, begin(5, 1000, 1, 7) + begin(5, 1100, 2, 8) +
                       end(5, 1200, 1) + end(5, 1300, 2));
  const auto result = run_all(trace);

  CHECK(result.diagnostics.incomplete_executions == 0);
  CHECK(result.diagnostics.orphan_end_processing == 0);
  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 2);
  const auto src = execs.ints("src_pe");
  const auto start = execs.ints("start_time_us");
  const auto end_t = execs.ints("end_time_us");
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i] == 1) {
      CHECK(start[i] == 1000);
      CHECK(end_t[i] == 1200);
    } else {
      CHECK(src[i] == 2);
      CHECK(start[i] == 1100);
      CHECK(end_t[i] == 1300);
    }
  }

  SECTION("an END naming another entry method does not close the BEGIN") {
    TempTrace other(kSts);
    other.add_projrc(0, 0);
    other.add_log(0, begin(6, 1000, 0, 7) + "3 0 10 1200 6 0 0 0\n");
    const auto r = run_all(other);
    CHECK(r.diagnostics.mismatched_end_processing == 1);
    CHECK(r.diagnostics.incomplete_executions == 1);
    ParquetTable ex(other.out_dir() + "/execution.parquet");
    REQUIRE(ex.rows() == 1);
    CHECK_FALSE(ex.ints("end_time_us")[0].has_value());
  }
}

TEST_CASE("A truncated record is skipped, not filled with zeros",
          "[log_parser][malformed][regression]") {
  // The log ends in the middle of an END_PROCESSING. Reading it with default
  // zeros would close the execution with cpu_duration_us = -100.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, begin(5, 1000, 0, 7, "0", 100) + "3 0 11 1200 5");
  const auto result = run_all(trace);

  CHECK(result.diagnostics.malformed_records == 1);
  CHECK(result.diagnostics.incomplete_executions == 1);
  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 1);
  CHECK_FALSE(execs.ints("end_time_us")[0].has_value());
  CHECK_FALSE(execs.ints("cpu_duration_us")[0].has_value());

  SECTION("a truncated BEGIN_PROCESSING creates no execution or instance") {
    TempTrace other(kSts);
    other.add_projrc(0, 0);
    other.add_log(0, "2 0 11 1000 5 0 64");
    const auto r = run_all(other);
    CHECK(r.diagnostics.malformed_records == 1);
    CHECK(ParquetTable(other.out_dir() + "/execution.parquet").rows() == 0);
    CHECK(ParquetTable(other.out_dir() + "/chare_instance.parquet").rows() ==
          0);
  }

  SECTION("a truncated CREATION yields no message") {
    TempTrace other(kSts);
    other.add_projrc(0, 0);
    other.add_log(0, "1 0 11 2000 5 0");
    const auto r = run_all(other);
    CHECK(r.diagnostics.malformed_records == 1);
    CHECK(ParquetTable(other.out_dir() + "/message.parquet").rows() == 0);
  }
}

TEST_CASE("Idle state is explicit", "[log_parser][idle]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);

  SECTION("an unmatched BEGIN_IDLE at end of log keeps a NULL end") {
    trace.add_log(0, "14 100 0\n15 300 0\n14 400 0\n");
    const auto result = run_all(trace);
    CHECK(result.diagnostics.incomplete_idle_intervals == 1);
    ParquetTable idle(trace.out_dir() + "/idle_interval.parquet");
    REQUIRE(idle.rows() == 2);
    CHECK(idle.ints("duration_us")[0] == 200);
    CHECK(idle.ints("start_time_us")[1] == 400);
    CHECK_FALSE(idle.ints("end_time_us")[1].has_value());
    CHECK_FALSE(idle.ints("duration_us")[1].has_value());
  }

  SECTION("an END_IDLE with nothing open neither fabricates a start nor "
          "reuses the previous interval") {
    trace.add_log(0, "14 100 0\n15 300 0\n15 500 0\n");
    const auto result = run_all(trace);
    CHECK(result.diagnostics.orphan_end_idle == 1);
    ParquetTable idle(trace.out_dir() + "/idle_interval.parquet");
    REQUIRE(idle.rows() == 1);
    CHECK(idle.ints("end_time_us")[0] == 300);
  }

  SECTION("a BEGIN_IDLE inside an open interval closes the earlier one with "
          "a NULL end") {
    trace.add_log(0, "14 100 0\n14 200 0\n15 300 0\n");
    run_all(trace);
    ParquetTable idle(trace.out_dir() + "/idle_interval.parquet");
    REQUIRE(idle.rows() == 2);
    CHECK(idle.ints("start_time_us")[0] == 100);
    CHECK_FALSE(idle.ints("end_time_us")[0].has_value());
    CHECK(idle.ints("start_time_us")[1] == 200);
    CHECK(idle.ints("end_time_us")[1] == 300);
  }
}

TEST_CASE("Every valid PE log yields one ProcessingElement row",
          "[reconstruction][processing_element]") {
  TempTrace trace(kSts);
  trace.add_projrc(1000, 9000);
  trace.add_log(0, "6 2000\n7 8000\n");
  trace.add_log(1, "6 2100\n");
  trace.add_log(2, begin(1, 3000, 0, 7) + end(1, 3100, 0));
  const auto result = run_all(trace);

  CHECK(result.diagnostics.logs_without_begin_computation == 1);
  CHECK(result.diagnostics.logs_without_end_computation == 2);
  ParquetTable pes(trace.out_dir() + "/processing_element.parquet");
  REQUIRE(pes.rows() == 3);
  const auto b = pes.ints("begin_time_us");
  const auto e = pes.ints("end_time_us");
  const auto d = pes.ints("computation_duration_us");
  const auto g = pes.ints("global_start_us");
  const auto a = pes.ints("aligned_begin_us");
  // Computation bounds are the markers as written, in the same frame as every
  // other stored timestamp; global_start_us records the shift the runtime
  // applied.
  CHECK(b[0] == 2000);
  CHECK(e[0] == 8000);
  CHECK(d[0] == 6000);
  CHECK(g[0] == 1000);
  // The log already carries the runtime's shift, so the aligned value is the
  // stored marker, not the marker minus the offset again.
  CHECK(a[0] == 2000);
  CHECK(pes.ints("total_pes")[0] == 3);
  CHECK(b[1] == 2100);
  CHECK_FALSE(e[1].has_value());
  CHECK_FALSE(d[1].has_value());
  CHECK(a[1] == 2100);
  CHECK_FALSE(b[2].has_value());
  CHECK_FALSE(e[2].has_value());
  CHECK_FALSE(d[2].has_value());
  CHECK_FALSE(a[2].has_value());
  CHECK(g[2] == 1000);
}

TEST_CASE("A unicast links its creation to the receiver's BEGIN_PROCESSING",
          "[reconstruction][message]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, creation(5, 2000, 7, 0));
  trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 1);
  const auto &m = rows[0];
  CHECK(m.src_pe == 0);
  CHECK(m.event == 5);
  CHECK(m.send == 2000);
  // The creation's last field is the interval creationDone() measured on the
  // source: the send completed 7 us after the CREATION.
  CHECK(m.s2e == 7);
  CHECK(m.enqueue == 2007);
  CHECK_FALSE(m.is_broadcast);
  CHECK_FALSE(m.fanout.has_value());
  CHECK(m.dst == 1);
  CHECK(m.exec == 2100);
  // irecvtime 0 is what the runtime writes; it is a value, not an absence.
  CHECK(m.recv == 0);
  CHECK(m.e2e == 93);
  CHECK(m.end2end == 100);
}

TEST_CASE("An unmatched unicast is kept as a partial row",
          "[reconstruction][message]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, creation(5, 2000, 3, 0));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].s2e == 3);
  CHECK(rows[0].enqueue == 2003);
  CHECK_FALSE(rows[0].dst.has_value());
  CHECK_FALSE(rows[0].recv.has_value());
  CHECK_FALSE(rows[0].exec.has_value());
  CHECK_FALSE(rows[0].e2e.has_value());
  CHECK_FALSE(rows[0].end2end.has_value());
}

TEST_CASE("A multicast yields one row per explicit destination",
          "[reconstruction][message][multicast]") {
  // Destinations 1 and 2; only PE 1 logs a matching BEGIN_PROCESSING. The
  // receiver lookup must retain both PEs' records under (src_pe, event)
  // rather than one, and the missing receiver stays a partial row.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, mcast(5, 2000, 4, 0, "1 2", 2));
  trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0));
  trace.add_log(2, begin(8, 2300, 0, 9) + end(8, 2400, 0));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 2);
  std::map<int64_t, MessageRow> by_dst;
  for (const auto &r : rows) {
    REQUIRE(r.dst.has_value());
    by_dst.emplace(*r.dst, r);
  }
  REQUIRE(by_dst.count(1) == 1);
  REQUIRE(by_dst.count(2) == 1);
  CHECK(by_dst.at(1).exec == 2100);
  CHECK(by_dst.at(1).end2end == 100);
  CHECK_FALSE(by_dst.at(1).is_broadcast);
  CHECK_FALSE(by_dst.at(1).fanout.has_value());
  CHECK_FALSE(by_dst.at(2).exec.has_value());
  CHECK_FALSE(by_dst.at(2).end2end.has_value());
  CHECK(by_dst.at(2).s2e == 4);
}

TEST_CASE("Multicast receivers on several PEs are all linked",
          "[reconstruction][message][multicast][regression]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, mcast(5, 2000, 0, 0, "1 2", 2));
  trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0));
  trace.add_log(2, begin(5, 2300, 0, 8) + end(5, 2400, 0));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 2);
  std::map<int64_t, int64_t> exec_by_dst;
  for (const auto &r : rows)
    exec_by_dst[*r.dst] = *r.exec;
  CHECK(exec_by_dst.at(1) == 2100);
  CHECK(exec_by_dst.at(2) == 2300);
}

TEST_CASE("A broadcast is one row and is never matched",
          "[reconstruction][message][broadcast]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, bcast(5, 2000, 2, 0, 3));
  // A receiver carrying the broadcast's pair exists, and must be ignored.
  trace.add_log(1, "2 0 10 2100 5 0 64 0 1 2 3 4 0\n"
                   "3 0 10 2200 5 0 0 0\n");
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].is_broadcast);
  CHECK(rows[0].fanout == 3);
  CHECK(rows[0].s2e == 2);
  CHECK(rows[0].enqueue == 2002);
  CHECK_FALSE(rows[0].dst.has_value());
  CHECK_FALSE(rows[0].recv.has_value());
  CHECK_FALSE(rows[0].exec.has_value());
  CHECK_FALSE(rows[0].e2e.has_value());
  CHECK_FALSE(rows[0].end2end.has_value());
}

TEST_CASE("Source-local serials do not collide across PEs",
          "[reconstruction][message][regression]") {
  // Serial 5 on PE 0 and serial 5 on PE 1 are unrelated messages; both land
  // on PE 2, which records the sender in each BEGIN_PROCESSING's pe field.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, creation(5, 1000, 0, 0));
  trace.add_log(1, creation(5, 3000, 0, 1));
  trace.add_log(2, begin(5, 1100, 0, 7) + end(5, 1150, 0) +
                       begin(5, 3100, 1, 8) + end(5, 3150, 1));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 2);
  std::map<int64_t, MessageRow> by_src;
  for (const auto &r : rows)
    by_src.emplace(r.src_pe, r);
  CHECK(by_src.at(0).exec == 1100);
  CHECK(by_src.at(0).end2end == 100);
  CHECK(by_src.at(1).exec == 3100);
  CHECK(by_src.at(1).end2end == 100);
}

TEST_CASE("Linkage does not depend on the order the logs are read",
          "[reconstruction][message][regression]") {
  // The receiver's log is listed before the sender's.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0));
  trace.add_log(0, creation(5, 2000, 0, 0));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].dst == 1);
  CHECK(rows[0].end2end == 100);
}

TEST_CASE("Repeated receivers on one PE are folded only for thread resumes",
          "[reconstruction][message]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);

  SECTION("resumes of a threaded entry method: one delivery, earliest wins") {
    // beginExecute(CmiObjId*) logs every resume under dummy_thread_ep and the
    // serial of the message that started the thread, on the same element.
    trace.add_log(0, "1 0 0 2000 5 0 64 0\n");
    trace.add_log(1, "2 0 0 2500 5 0 0 0 7 0\n3 0 0 2600 5 0 0 0\n"
                     "2 0 0 2100 5 0 0 0 7 0\n3 0 0 2200 5 0 0 0\n");
    run_all(trace);
    const auto rows = read_messages(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].dst == 1);
    CHECK(rows[0].exec == 2100);
  }

  SECTION("thread-entry records on different elements are not one thread") {
    trace.add_log(0, "1 0 0 2000 5 0 64 0\n");
    trace.add_log(1, "2 0 0 2100 5 0 0 0 7 0\n3 0 0 2200 5 0 0 0\n"
                     "2 0 0 2300 5 0 0 0 8 0\n3 0 0 2400 5 0 0 0\n");
    run_all(trace);
    const auto rows = read_messages(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].dst == 1);
    CHECK_FALSE(rows[0].exec.has_value());
  }

  SECTION("identical identity on an ordinary entry method is unresolved") {
    // Two executions of element 7 with the same length under one pair. The
    // trace cannot say which processed the message, so neither is chosen.
    trace.add_log(0, creation(5, 2000, 0, 0));
    trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0) +
                         begin(5, 2500, 0, 7) + end(5, 2600, 0));
    run_all(trace);
    const auto rows = read_messages(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].dst == 1);
    CHECK_FALSE(rows[0].exec.has_value());
    CHECK_FALSE(rows[0].recv.has_value());
    CHECK_FALSE(rows[0].end2end.has_value());
    CHECK(rows[0].s2e == 0);
  }

  SECTION("different chare instances are unresolved") {
    trace.add_log(0, creation(5, 2000, 0, 0));
    trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0) +
                         begin(5, 2300, 0, 8) + end(5, 2400, 0));
    run_all(trace);
    const auto rows = read_messages(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].dst == 1);
    CHECK_FALSE(rows[0].exec.has_value());
  }
}

TEST_CASE("An unavailable receive time is NULL and a zero one is not",
          "[reconstruction][message][execution]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, creation(5, 2000, 0, 0) + creation(6, 3000, 0, 0));
  trace.add_log(1, begin(5, 2100, 0, 7, kUnavailable) + end(5, 2200, 0) +
                       begin(6, 3100, 0, 7, "0") + end(6, 3200, 0));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 2);
  std::map<int64_t, MessageRow> by_event;
  for (const auto &r : rows)
    by_event.emplace(r.event, r);
  CHECK_FALSE(by_event.at(5).recv.has_value());
  CHECK(by_event.at(5).exec == 2100);
  CHECK(by_event.at(6).recv == 0);

  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 2);
  CHECK_FALSE(execs.ints("recv_time_us")[0].has_value());
  CHECK_FALSE(execs.ints("queue_wait_us")[0].has_value());
  CHECK(execs.ints("recv_time_us")[1] == 0);
  CHECK(execs.ints("queue_wait_us")[1] == 3100);
}

TEST_CASE("A nonzero start offset is not subtracted again",
          "[reconstruction][alignment][regression]") {
  // LogPool::setNewStartTime() already rebased every record by
  // RC_GLOBAL_START_TIME before the log was written; subtracting it here
  // would shift the run a second time and put ProcessingElement's marker
  // bounds in a different frame from the executions.
  TempTrace trace(kSts);
  trace.add_projrc(1000, 9000);
  trace.add_log(0, creation(5, 2000, 7, 0) + "6 1500\n14 2500 0\n15 2600 0\n");
  trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0));
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].send == 2000);
  CHECK(rows[0].enqueue == 2007);
  CHECK(rows[0].exec == 2100);
  CHECK(rows[0].s2e == 7);
  CHECK(rows[0].e2e == 93);
  CHECK(rows[0].end2end == 100);
  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  CHECK(execs.ints("start_time_us")[0] == 2100);
  CHECK(execs.ints("end_time_us")[0] == 2200);
  CHECK(execs.ints("wall_duration_us")[0] == 100);
  ParquetTable idle(trace.out_dir() + "/idle_interval.parquet");
  CHECK(idle.ints("start_time_us")[0] == 2500);
  CHECK(idle.ints("end_time_us")[0] == 2600);
  ParquetTable pes(trace.out_dir() + "/processing_element.parquet");
  const auto pe_ids = pes.ints("pe_id");
  for (size_t i = 0; i < pe_ids.size(); ++i) {
    if (pe_ids[i] == 0) {
      CHECK(pes.ints("begin_time_us")[i] == 1500);
      CHECK(pes.ints("aligned_begin_us")[i] == 1500);
      CHECK(pes.ints("global_start_us")[i] == 1000);
    }
  }
}

TEST_CASE("Without a .projrc cross-PE derivations are suppressed",
          "[reconstruction][alignment][clock]") {
  // No add_projrc: the reference is unavailable, as opposed to zero. Local
  // records and identifier-based linkage are kept; only the quantities that
  // compare two PEs' clocks are withheld.
  TempTrace trace(kSts);
  trace.add_log(0, creation(5, 2000, 7, 0) + creation(6, 4000, 1, 0) +
                       begin(6, 4200, 0, 9) + end(6, 4300, 0) + "6 1500\n");
  trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0) +
                       begin(9, 5000, 0, 9) + end(9, 5100, 0));
  const auto rc = charmvz::parse_rc_file(trace.rc_path());
  CHECK_FALSE(rc.clock_reference_usable());
  run_all(trace);

  const auto rows = read_messages(trace.out_dir());
  REQUIRE(rows.size() == 2);
  std::map<int64_t, MessageRow> by_event;
  for (const auto &r : rows)
    by_event.emplace(r.event, r);
  // Cross-PE: linked, but no delay across clocks.
  CHECK(by_event.at(5).dst == 1);
  CHECK(by_event.at(5).exec == 2100);
  CHECK(by_event.at(5).s2e == 7);
  CHECK(by_event.at(5).enqueue == 2007);
  CHECK_FALSE(by_event.at(5).e2e.has_value());
  CHECK_FALSE(by_event.at(5).end2end.has_value());
  // A PE sending to itself compares one clock with itself.
  CHECK(by_event.at(6).dst == 0);
  CHECK(by_event.at(6).e2e == 199);
  CHECK(by_event.at(6).end2end == 200);

  // Element 9 ran on PE 0 then PE 1, but the order of two clocks with no
  // common reference is unknown.
  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  CHECK(mig.rows() == 0);

  ParquetTable pes(trace.out_dir() + "/processing_element.parquet");
  REQUIRE(pes.rows() == 2);
  CHECK(pes.ints("begin_time_us")[0] == 1500);
  CHECK_FALSE(pes.ints("global_start_us")[0].has_value());
  CHECK_FALSE(pes.ints("aligned_begin_us")[0].has_value());
}

TEST_CASE("A .projrc with a zero offset is a usable clock reference",
          "[reconstruction][alignment][clock]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 5000);
  const auto rc = charmvz::parse_rc_file(trace.rc_path());
  CHECK(rc.clock_reference_usable());
  CHECK(rc.global_start_time_us == 0);
  trace.add_log(0, begin(9, 1000, 0, 9) + end(9, 1100, 0));
  trace.add_log(1, begin(9, 2000, 0, 9) + end(9, 2100, 0));
  run_all(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  REQUIRE(mig.rows() == 1);
  CHECK(mig.ints("gap_us")[0] == 900);
  ParquetTable pes(trace.out_dir() + "/processing_element.parquet");
  CHECK(pes.ints("global_start_us")[0] == 0);
}

TEST_CASE("A .projrc without RC_GLOBAL_START_TIME is unusable",
          "[reconstruction][clock]") {
  TempTrace trace(kSts);
  {
    std::ofstream rc(trace.rc_path());
    rc << "RC_GLOBAL_END_TIME 5000\n";
  }
  const auto rc = charmvz::parse_rc_file(trace.rc_path());
  CHECK_FALSE(rc.clock_reference_usable());
}

TEST_CASE("An incomplete execution is a location but not a migration source",
          "[reconstruction][migration][incomplete]") {
  // Element 7: PE 0 (complete), PE 1 (never ended), PE 0 (complete). Skipping
  // the incomplete one would chain PE 0 to PE 0 and hide both hops; using
  // it as a source would need an end it does not have. The hop into PE 1 is
  // written; the hop out of it is counted in the ordinal and withheld.
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, begin(1, 1000, 0, 7) + end(1, 1200, 0) +
                       begin(2, 3000, 0, 7) + end(2, 3200, 0));
  trace.add_log(1, begin(1, 2000, 0, 7));
  run_all(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  REQUIRE(mig.rows() == 1);
  CHECK(mig.ints("src_pe")[0] == 0);
  CHECK(mig.ints("dst_pe")[0] == 1);
  CHECK(mig.ints("last_exec_end_src_us")[0] == 1200);
  CHECK(mig.ints("first_exec_start_dst_us")[0] == 2000);
  CHECK(mig.ints("migration_seq")[0] == 1);
}

TEST_CASE("A full run writes all thirteen tables with the frozen schemas",
          "[reconstruction][schema]") {
  TempTrace trace(kSts);
  trace.add_projrc(0, 0);
  trace.add_log(0, "6 0\n" + creation(5, 2000, 0, 0) + "7 9000\n");
  trace.add_log(1, begin(5, 2100, 0, 7) + end(5, 2200, 0));
  run_all(trace);

  const std::map<std::string, std::shared_ptr<arrow::Schema>> expected = {
      {"processing_element", charmvz::schema::processing_element()},
      {"chare_collection", charmvz::schema::chare_collection()},
      {"entry_method", charmvz::schema::entry_method()},
      {"message_type", charmvz::schema::message_type()},
      {"chare_instance", charmvz::schema::chare_instance()},
      {"execution", charmvz::schema::execution()},
      {"message", charmvz::schema::message()},
      {"idle_interval", charmvz::schema::idle_interval()},
      {"migration_episode", charmvz::schema::migration_episode()},
      {"user_event", charmvz::schema::user_event()},
      {"simulation_step", charmvz::schema::simulation_step()},
      {"user_stat", charmvz::schema::user_stat()},
      {"memory_sample", charmvz::schema::memory_sample()},
  };
  REQUIRE(expected.size() == 13);
  for (const auto &[name, schema] : expected) {
    INFO(name);
    const auto path = trace.out_dir() + "/" + name + ".parquet";
    REQUIRE(std::filesystem::exists(path));
    ParquetTable table(path);
    const auto actual = table.schema();
    REQUIRE(actual->num_fields() == schema->num_fields());
    for (int i = 0; i < schema->num_fields(); ++i) {
      INFO(schema->field(i)->name());
      CHECK(actual->field(i)->name() == schema->field(i)->name());
      CHECK(actual->field(i)->type()->Equals(schema->field(i)->type()));
      CHECK(actual->field(i)->nullable() == schema->field(i)->nullable());
    }
  }
  // The expected-empty tables exist with zero rows rather than not at all.
  CHECK(ParquetTable(trace.out_dir() + "/migration_episode.parquet").rows() ==
        0);
  CHECK(ParquetTable(trace.out_dir() + "/simulation_step.parquet").rows() == 0);
  CHECK(ParquetTable(trace.out_dir() + "/user_stat.parquet").rows() == 0);
  CHECK(ParquetTable(trace.out_dir() + "/memory_sample.parquet").rows() == 0);
  CHECK(ParquetTable(trace.out_dir() + "/message_type.parquet").rows() == 1);
}
