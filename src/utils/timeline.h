#ifndef TIMELINE_H
#define TIMELINE_H
#include <string>
#include <vector>

// Constants for special entry points (matching Projections Analysis.java)
static constexpr int64_t IDLE_ENTRY_POINT = -1;
static constexpr int64_t OVERHEAD_ENTRY_POINT = -2;

struct TimelineMessage {
  int64_t send_time;
  int64_t recv_time;
  int64_t msg_id;
  int64_t size;
};

struct PackTime {
  int64_t begin_time;
  int64_t end_time;

  PackTime(int64_t bt) : begin_time(bt), end_time(-1) {}
};

/**
 * TimelineEvent represents a single timeline entry in the execution.
 * This matches the Java TimelineEvent class from Projections.
 */
struct TimelineEvent {
  int64_t begin_time;
  int64_t end_time;
  int64_t recv_time;
  int64_t event_id;
  int64_t pe;
  int64_t entry_point;
  int64_t message_size;
  int64_t cpu_begin;
  int64_t cpu_end;

  // Additional data from log entries
  int64_t mtype;
  std::vector<TimelineMessage> messages;
  std::vector<PackTime> pack_times;

  // User event association
  std::string user_event_name;

  TimelineEvent()
      : begin_time(-1), end_time(-1), recv_time(-1), event_id(-1), pe(-1),
        entry_point(-1), message_size(-1), cpu_begin(-1), cpu_end(-1),
        mtype(-1) {}

  TimelineEvent(int64_t bt, int64_t et, int64_t ep, int64_t processor)
      : begin_time(bt), end_time(et), recv_time(-1), event_id(-1),
        pe(processor), entry_point(ep), message_size(-1), cpu_begin(-1),
        cpu_end(-1), mtype(-1) {}

  TimelineEvent(int64_t bt, int64_t et, int64_t ep, int64_t processor,
                int64_t msg_len)
      : begin_time(bt), end_time(et), recv_time(-1), event_id(-1),
        pe(processor), entry_point(ep), message_size(msg_len), cpu_begin(-1),
        cpu_end(-1), mtype(-1) {}

  void add_message(const TimelineMessage &msg) { messages.push_back(msg); }

  void add_pack_time(const PackTime &pack) { pack_times.push_back(pack); }

  bool is_idle_event() const { return entry_point == IDLE_ENTRY_POINT; }

  bool is_overhead_event() const { return entry_point == OVERHEAD_ENTRY_POINT; }
};

struct Timeline {
  int64_t log_id;
  std::vector<TimelineEvent> events;

  // Statistics
  size_t total_events_processed = 0;
  size_t timeline_events_created = 0;
  size_t events_filtered_by_duration = 0;
};

/**
 * Create a timeline from a log file using the Projections algorithm.
 * This implements the core state machine from LogLoader.createtimeline()
 *
 * @param log_file_path Path to the compressed log file
 * @param begin_time Start time for timeline (use 0 for complete trace)
 * @param end_time End time for timeline (use LLONG_MAX for complete trace)
 * @param min_entry_duration Minimum duration for events to be included
 * @return Complete timeline with all events
 */
// Backward-compatible overload (preserves original API)
auto create_timeline(const std::string_view log_file_path) -> Timeline;

// Extended version with time range and filtering
auto create_timeline(const std::string_view log_file_path, int64_t begin_time,
                     int64_t end_time, int64_t min_entry_duration) -> Timeline;

#endif