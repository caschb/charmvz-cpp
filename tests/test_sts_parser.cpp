#include "sts_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Writes an STS file to a unique temporary path and removes it on scope exit,
// so each test case can be written against exactly the records it cares about.
class TempStsFile {
public:
  explicit TempStsFile(const std::string &contents) {
    path_ = std::filesystem::temp_directory_path() /
            ("charmvz_sts_test_" + std::to_string(counter_++) + ".sts");
    std::ofstream out(path_);
    out << contents;
  }

  ~TempStsFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  TempStsFile(const TempStsFile &) = delete;
  auto operator=(const TempStsFile &) -> TempStsFile & = delete;

  [[nodiscard]] auto path() const -> std::string { return path_.string(); }

private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

// The projections-specific block always precedes the user-event block, and
// VERSION is mandatory, so every fixture needs this preamble.
constexpr auto kPreamble = "PROJECTIONS_ID \nVERSION 11.0\n";

} // namespace

TEST_CASE("STS user events are parsed", "[sts][user_event]") {
  SECTION("EVENT records populate both the vector and the lookup map") {
    TempStsFile sts(std::string(kPreamble) + "TOTAL_EVENTS 2\n"
                                             "TOTAL_STATS 0\n"
                                             "EVENT 0 StepBegin\n"
                                             "EVENT 1 StepEnd\n"
                                             "END\n");

    const auto data = charmvz::parse_sts_file(sts.path());

    REQUIRE(data.total_user_events == 2);
    REQUIRE(data.user_events.size() == 2);
    CHECK(data.user_events[0].event_id == 0);
    CHECK(data.user_events[0].name == "StepBegin");
    CHECK(data.user_events[1].event_id == 1);
    CHECK(data.user_events[1].name == "StepEnd");

    REQUIRE(data.user_event_map.contains(1));
    CHECK(data.user_event_map.at(1).name == "StepEnd");
  }

  SECTION("names may contain spaces") {
    // Charm++ writes the name with a bare `%s` and no quoting, so a name
    // registered as "Load balancing phase" occupies the rest of the line.
    TempStsFile sts(std::string(kPreamble) + "EVENT 7 Load balancing phase\n"
                                             "END\n");

    const auto data = charmvz::parse_sts_file(sts.path());

    REQUIRE(data.user_events.size() == 1);
    CHECK(data.user_events[0].name == "Load balancing phase");
  }

  SECTION("a duplicate id keeps the first registration") {
    // Matches Projections' StsReader, which ignores a repeated key.
    TempStsFile sts(std::string(kPreamble) + "EVENT 3 First\n"
                                             "EVENT 3 Second\n"
                                             "END\n");

    const auto data = charmvz::parse_sts_file(sts.path());

    REQUIRE(data.user_events.size() == 1);
    CHECK(data.user_events[0].name == "First");
    CHECK(data.user_event_map.at(3).name == "First");
  }

  SECTION("event ids need not be contiguous or ordered") {
    // The id is whatever the application passed to traceRegisterUserEvent();
    // it is not an index into the table.
    TempStsFile sts(std::string(kPreamble) + "EVENT 900 Late\n"
                                             "EVENT 4 Early\n"
                                             "END\n");

    const auto data = charmvz::parse_sts_file(sts.path());

    REQUIRE(data.user_events.size() == 2);
    CHECK(data.user_event_map.at(900).name == "Late");
    CHECK(data.user_event_map.at(4).name == "Early");
  }

  SECTION("a trace with no user events yields an empty table") {
    // This is what the LeanMD reference traces look like.
    TempStsFile sts(std::string(kPreamble) + "TOTAL_EVENTS 0\n"
                                             "TOTAL_STATS 0\n"
                                             "END\n");

    const auto data = charmvz::parse_sts_file(sts.path());

    CHECK(data.total_user_events == 0);
    CHECK(data.user_events.empty());
    CHECK(data.user_event_map.empty());
  }
}

TEST_CASE("STS user stats are parsed", "[sts][user_stat]") {
  TempStsFile sts(std::string(kPreamble) + "TOTAL_STATS 2\n"
                                           "STAT 0 particles moved\n"
                                           "STAT 1 EnergyDrift\n"
                                           "END\n");

  const auto data = charmvz::parse_sts_file(sts.path());

  REQUIRE(data.total_user_stats == 2);
  REQUIRE(data.user_stats.size() == 2);
  CHECK(data.user_stats[0].name == "particles moved");
  CHECK(data.user_stats[1].name == "EnergyDrift");
  CHECK(data.user_stat_map.at(1).name == "EnergyDrift");
}

TEST_CASE("EVENT parsing does not disturb the other STS records",
          "[sts][regression]") {
  // TOTAL_EVENTS vs TOTAL_PAPI_EVENTS, and EVENT vs PAPI_EVENT, are distinct
  // tokens that a prefix-based match would confuse.
  TempStsFile sts(std::string(kPreamble) + "TOTAL_PHASES 1\n"
                                           "PROCESSORS 80\n"
                                           "TOTAL_PAPI_EVENTS 2\n"
                                           "PAPI_EVENT 0 PAPI_TOT_INS\n"
                                           "PAPI_EVENT 1 PAPI_TOT_CYC\n"
                                           "TOTAL_EVENTS 1\n"
                                           "CHARE 42 \"Cell\" 3\n"
                                           "CHARE 43 \"Compute\" 6\n"
                                           "ENTRY CHARE 144 \"Cell()\" 42 0\n"
                                           "MESSAGE 27 16\n"
                                           "TOTAL_STATS 0\n"
                                           "EVENT 5 Timestep\n"
                                           "END\n");

  const auto data = charmvz::parse_sts_file(sts.path());

  CHECK(data.total_pes == 80);
  CHECK(data.total_papi_events == 2);
  REQUIRE(data.papi_event_names.size() == 2);
  CHECK(data.papi_event_names[0] == "PAPI_TOT_INS");
  CHECK(data.papi_event_names[1] == "PAPI_TOT_CYC");

  CHECK(data.total_user_events == 1);
  REQUIRE(data.user_events.size() == 1);
  CHECK(data.user_events[0].name == "Timestep");

  // The chare-index arity fix depends on ndims surviving intact.
  REQUIRE(data.chare_map.contains(42));
  CHECK(data.chare_map.at(42).ndims == 3);
  CHECK(data.chare_map.at(43).ndims == 6);
  REQUIRE(data.ep_map.contains(144));
  CHECK(data.ep_map.at(144).collection_id == 42);
  CHECK(data.message_map.at(27).size == 16);
}

TEST_CASE("An unsupported STS VERSION is rejected", "[sts]") {
  TempStsFile sts("PROJECTIONS_ID \nVERSION 10.0\nEND\n");
  CHECK_THROWS(charmvz::parse_sts_file(sts.path()));
}
