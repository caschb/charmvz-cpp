#include "timeline.h"
#include "log_entry.h"
#include "log_reader.h"
#include <climits>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <regex>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace {
auto extract_log_id(const std::string_view &log_file_path) {
  std::string log_file_name =
      std::filesystem::path(log_file_path).filename().string();
  std::smatch match;
  const std::regex log_regex(R"(.*\.prj\.(\d+)\.log\.gz$)");
  int64_t id = -1;
  if (std::regex_match(log_file_name, match, log_regex)) {
    id = std::stoi(match[1]);
  } else {
    spdlog::warn("Failed to extract log ID from log file name {}",
                 log_file_name);
  }
  return id;
}

} // namespace

auto create_timeline(const std::string_view log_file_path, int64_t begin_time,
                     int64_t end_time, int64_t min_entry_duration) -> Timeline {
  Timeline timeline;
  timeline.log_id = extract_log_id(log_file_path);

  spdlog::debug(
      "Creating timeline from log {} (time range: {} - {}, min duration: {})",
      log_file_path, begin_time, end_time, min_entry_duration);

  LogReader log_reader(log_file_path);

  // State machine variables (matching Projections LogLoader.createtimeline)
  bool is_processing = false;
  TimelineEvent *current_timeline_event = nullptr;
  std::unique_ptr<TimelineEvent> current_te_holder;
  PackTime *current_pack_time = nullptr;

  // Track statistics
  timeline.total_events_processed = 0;
  timeline.timeline_events_created = 0;
  timeline.events_filtered_by_duration = 0;

  while (log_reader.hasNextEntry()) {
    auto log_entry = log_reader.nextEntry();
    timeline.total_events_processed++;

    // Skip events outside time range for non-idle events
    if (log_entry.entry_point != IDLE_ENTRY_POINT) {
      if (log_entry.timestamp < begin_time || log_entry.timestamp > end_time) {
        continue;
      }
    }

    spdlog::trace("Processing log entry: type={}, entry={}, time={}, pe={}",
                  static_cast<int>(log_entry.type), log_entry.entry_point,
                  log_entry.timestamp, log_entry.pe);
    bool temp_te = false;
    switch (log_entry.type) {

    case LogType::BEGIN_COMPUTATION: {
      spdlog::trace("BEGIN_COMPUTATION: entry={}, pe={}, time={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp);
      break;
    }

    case LogType::END_COMPUTATION: {
      spdlog::trace("END_COMPUTATION: entry={}, pe={}, time={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp);
      if (is_processing) {
        // Add a "pretend" end event to accommodate the prior begin processing
        if (current_timeline_event != nullptr) {
          current_timeline_event->end_time = log_entry.timestamp;

          // If the entry was not long enough, remove it from the timeline
          if (current_timeline_event->end_time -
                  current_timeline_event->begin_time <
              min_entry_duration) {
            spdlog::trace(
                "Filtered out short event: duration={}, begin={}, end={}",
                current_timeline_event->end_time -
                    current_timeline_event->begin_time,
                current_timeline_event->begin_time,
                current_timeline_event->end_time);
            if (!timeline.events.empty()) {
              timeline.events.pop_back();
              timeline.events_filtered_by_duration++;
            }
          }
        }
        current_timeline_event = nullptr;
        current_te_holder.reset();
        is_processing = false;
      }
      break;
    }

    case LogType::BEGIN_PROCESSING: {
      spdlog::trace("BEGIN_PROCESSING: entry={}, pe={}, time={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp);
      if (is_processing) {
        // Add a "pretend" end event to accommodate the prior begin processing
        // event
        if (current_timeline_event != nullptr) {
          current_timeline_event->end_time = log_entry.timestamp;

          // If the entry was not long enough, remove it from the timeline
          if (current_timeline_event->end_time -
                  current_timeline_event->begin_time <
              min_entry_duration) {
            spdlog::trace(
                "Filtered out short event: duration={}, begin={}, end={}",
                current_timeline_event->end_time -
                    current_timeline_event->begin_time,
                current_timeline_event->begin_time,
                current_timeline_event->end_time);
            if (!timeline.events.empty()) {
              timeline.events.pop_back();
              timeline.events_filtered_by_duration++;
            }
          }
        }
        current_timeline_event = nullptr;
        current_te_holder.reset();
      }

      is_processing = true;

      // Create new timeline event
      current_te_holder = std::make_unique<TimelineEvent>(
          log_entry.timestamp,   // begin_time
          log_entry.timestamp,   // end_time (temporary)
          log_entry.entry_point, // entry_point
          log_entry.pe,          // processor
          log_entry.mtype        // message size/type
      );
      current_te_holder->event_id = log_entry.event;
      current_te_holder->mtype = log_entry.mtype;

      if (current_te_holder->end_time == LLONG_MAX) {
        spdlog::warn(
            "BEGIN_COMPUTATION log entry has invalid end_time=LLONG_MAX");
      }
      timeline.events.push_back(*current_te_holder);
      current_timeline_event = &timeline.events.back();
      timeline.timeline_events_created++;

      spdlog::trace("Created timeline event: begin={}, entry={}, pe={}",
                    current_timeline_event->begin_time,
                    current_timeline_event->entry_point,
                    current_timeline_event->pe);
      break;
    }

    case LogType::END_PROCESSING: {
      spdlog::trace("END_PROCESSING: entry={}, pe={}, time={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp);

      if (current_timeline_event != nullptr) {
        current_timeline_event->end_time = log_entry.timestamp;

        // If the entry was not long enough, remove it from the timeline
        if (current_timeline_event->end_time -
                current_timeline_event->begin_time <
            min_entry_duration) {
          timeline.events.pop_back();
          timeline.events_filtered_by_duration++;
          spdlog::trace("Filtered out short event at end_processing: "
                        "duration={}, begin={}, end={}",
                        current_timeline_event->end_time -
                            current_timeline_event->begin_time,
                        current_timeline_event->begin_time,
                        current_timeline_event->end_time);
        } else {
          spdlog::trace("Completed timeline event: duration={}",
                        current_timeline_event->end_time -
                            current_timeline_event->begin_time);
        }
      }

      current_timeline_event = nullptr;
      current_te_holder.reset();
      is_processing = false;
      break;
    }

    case LogType::BEGIN_IDLE: {
      spdlog::trace("BEGIN_IDLE: pe={}, time={}", log_entry.pe,
                    log_entry.timestamp);

      // Create idle timeline event
      current_te_holder = std::make_unique<TimelineEvent>(
          log_entry.timestamp, // begin_time
          LLONG_MAX,           // end_time (will be set by END_IDLE)
          IDLE_ENTRY_POINT,    // entry_point
          -1                   // processor (idle doesn't belong to specific PE)
      );

      timeline.events.push_back(*current_te_holder);
      current_timeline_event = &timeline.events.back();
      timeline.timeline_events_created++;

      spdlog::trace("Created idle event: begin={}",
                    current_timeline_event->begin_time);
      break;
    }

    case LogType::END_IDLE: {
      spdlog::trace("END_IDLE: pe={}, time={}", log_entry.pe,
                    log_entry.timestamp);

      if (current_timeline_event != nullptr &&
          current_timeline_event->is_idle_event()) {
        current_timeline_event->end_time = log_entry.timestamp;
        spdlog::trace("Set idle event end_time={}",
                      current_timeline_event->end_time);
        // Check minimum duration for idle events too
        if (current_timeline_event->end_time -
                current_timeline_event->begin_time <
            min_entry_duration) {
          timeline.events.pop_back();
          timeline.events_filtered_by_duration++;
        }
      }
      spdlog::trace("Completed idle event, end_time={}",
                    current_timeline_event->end_time);
      current_timeline_event = nullptr;
      current_te_holder.reset();
      break;
    }

    case LogType::BEGIN_PACK: {
      spdlog::trace("BEGIN_PACK: pe={}, time={}", log_entry.pe,
                    log_entry.timestamp);

      // Start a new dummy event if we don't have one
      if (current_timeline_event == nullptr) {
        current_te_holder = std::make_unique<TimelineEvent>(
            log_entry.timestamp, log_entry.timestamp, OVERHEAD_ENTRY_POINT,
            log_entry.pe);
        timeline.events.push_back(*current_te_holder);
        current_timeline_event = &timeline.events.back();
        timeline.timeline_events_created++;
      }

      // Add pack time to current event
      const PackTime pack_time(log_entry.timestamp);
      current_timeline_event->add_pack_time(pack_time);
      current_pack_time = &current_timeline_event->pack_times.back();
      break;
    }

    case LogType::END_PACK: {
      spdlog::trace("END_PACK: pe={}, time={}", log_entry.pe,
                    log_entry.timestamp);
      if (current_pack_time != nullptr) {
        current_pack_time->end_time = log_entry.timestamp;
      }
      current_pack_time = nullptr;

      if (current_timeline_event != nullptr &&
          current_timeline_event->is_overhead_event()) {
        current_timeline_event = nullptr;
        current_te_holder.reset();
      }
      break;
    }

    case LogType::BEGIN_UNPACK: {
      spdlog::trace("BEGIN_UNPACK: pe={}, time={}", log_entry.pe,
                    log_entry.timestamp);
      break;
    }

    case LogType::END_UNPACK: {
      spdlog::trace("END_UNPACK: pe={}, time={}", log_entry.pe,
                    log_entry.timestamp);
      break;
    }

    case LogType::CREATION: {
      spdlog::trace("CREATION: entry={}, pe={}, time={}, msglen={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp,
                    log_entry.mtype);

      temp_te = false;
      if (current_timeline_event == nullptr) {
        // Create a temporary timeline event to hold messages until processing
        // begins
        current_te_holder = std::make_unique<TimelineEvent>(
            log_entry.timestamp, log_entry.timestamp, OVERHEAD_ENTRY_POINT,
            log_entry.pe, log_entry.mtype);
        timeline.events.push_back(*current_te_holder);
        current_timeline_event = &timeline.events.back();
        timeline.timeline_events_created++;
        temp_te = true;
      }
      // Create message record
      TimelineMessage message{};
      message.send_time = log_entry.timestamp;
      message.msg_id = log_entry.event;
      message.size = log_entry.mtype;

      // Add to current timeline event if we have one
      current_timeline_event->add_message(message);
      if (temp_te) {
        current_timeline_event = nullptr;
        current_te_holder.reset();
      }
      spdlog::trace(
          "CREATION: Added message to timeline event: send_time={}, msg_id={}, "
          "size={}",
          message.send_time, message.msg_id, message.size);
      break;
    }

    case LogType::USER_EVENT: {
      spdlog::trace("USER_EVENT: entry={}, pe={}, time={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp);
      // For now, just log user events - could be enhanced to store them
      break;
    }

    case LogType::END_PHASE: {
      spdlog::trace("END_PHASE: pe={}, time={}", log_entry.pe,
                    log_entry.timestamp);
      break;
    }

    case LogType::USER_EVENT_PAIR: {
      spdlog::trace("USER_EVENT_PAIR: entry={}, pe={}, time={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp);
      break;
    }

    case LogType::CREATION_BCAST: {
      spdlog::trace("CREATION_BCAST: entry={}, pe={}, time={}, msglen={}",
                    log_entry.entry_point, log_entry.pe, log_entry.timestamp,
                    log_entry.mtype);

      temp_te = false;
      if (current_timeline_event == nullptr) {
        // Create a temporary timeline event to hold messages until processing
        // begins
        current_te_holder = std::make_unique<TimelineEvent>(
            log_entry.timestamp, log_entry.timestamp, OVERHEAD_ENTRY_POINT,
            log_entry.pe, log_entry.mtype);
        timeline.events.push_back(*current_te_holder);
        current_timeline_event = &timeline.events.back();
        timeline.timeline_events_created++;
        temp_te = true;
      }
      // Create message record
      TimelineMessage message{};
      message.send_time = log_entry.timestamp;
      message.msg_id = log_entry.event;
      message.size = log_entry.mtype;

      // Add to current timeline event if we have one
      current_timeline_event->add_message(message);
      if (temp_te) {
        current_timeline_event = nullptr;
        current_te_holder.reset();
      }
      spdlog::trace(
          "CREATION_BCAST: Added message to timeline event: send_time={}, "
          "msg_id={}, size={}",
          message.send_time, message.msg_id, message.size);
      break;
    }

    case LogType::UNKNOWN:
    default: {
      spdlog::warn("Unknown or unhandled log entry type: {}",
                   static_cast<int>(log_entry.type));
      break;
    }
    }
  }

  // Handle any remaining open timeline event
  if (current_timeline_event != nullptr) {
    current_timeline_event->end_time = end_time;

    if (current_timeline_event->end_time - current_timeline_event->begin_time <
        min_entry_duration) {
      timeline.events.pop_back();
      timeline.events_filtered_by_duration++;
    }
  }

  spdlog::info("Timeline creation completed: {} events processed, {} timeline "
               "events created, {} filtered by duration",
               timeline.total_events_processed,
               timeline.timeline_events_created,
               timeline.events_filtered_by_duration);

  return timeline;
}

// Backward-compatible overload
auto create_timeline(const std::string_view log_file_path) -> Timeline {
  return create_timeline(log_file_path, 0, LLONG_MAX, 0);
}