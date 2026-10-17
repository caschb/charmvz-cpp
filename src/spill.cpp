#include "spill.h"
#include "memory_budget.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <regex>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

namespace charmvz {

namespace {

constexpr std::array<char, 8> kMagic = {'C', 'V', 'Z', 'S', 'P', 'I', 'L', 'L'};
constexpr uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderBytes = 24;
constexpr off_t kCountOffset = 16;

[[noreturn]] void fail(const std::string &what,
                       const std::filesystem::path &path, int error) {
  throw std::runtime_error(what + " " + path.string() + ": " +
                           std::strerror(error));
}

void write_all(int fd, const char *data, std::size_t size,
               const std::filesystem::path &path) {
  while (size > 0) {
    const ssize_t written = ::write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      fail("Cannot write spill run", path, errno);
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

uint64_t directory_counter = 0;

} // namespace

SpillDirectory::SpillDirectory(const std::filesystem::path &root, bool keep)
    : keep_(keep) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec))
    throw std::runtime_error("Spill directory root " + root.string() +
                             " is not a directory");
  path_ = root / (std::string(kPrefix) + std::to_string(::getpid()) + "-" +
                  std::to_string(directory_counter++));
  std::filesystem::remove_all(path_, ec);
  if (!std::filesystem::create_directory(path_, ec))
    throw std::runtime_error("Cannot create spill directory " + path_.string() +
                             ": " + ec.message());

  // Prove that runs can be written here before any parsing starts, rather
  // than after Stage 2 has done most of its work.
  const auto probe = path_ / "probe";
  const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  const int error = errno;
  if (fd < 0 || ::write(fd, kMagic.data(), kMagic.size()) !=
                    static_cast<ssize_t>(kMagic.size())) {
    if (fd >= 0)
      ::close(fd);
    std::filesystem::remove_all(path_, ec);
    fail("Cannot write in spill directory", path_, fd < 0 ? error : errno);
  }
  ::close(fd);
  std::filesystem::remove(probe, ec);
}

SpillDirectory::~SpillDirectory() {
  if (keep_) {
    spdlog::info("Spill directory kept: {}", path_.string());
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  if (ec)
    spdlog::warn("Could not remove spill directory {}: {}", path_.string(),
                 ec.message());
}

auto SpillDirectory::next_run_path() -> std::filesystem::path {
  return path_ / ("run-" + std::to_string(next_run_++) + ".bin");
}

auto remove_stale_spill_directories(const std::filesystem::path &root) -> int {
  static const std::regex name{std::string(SpillDirectory::kPrefix) +
                               R"((\d+)-\d+)"};
  int removed = 0;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
    std::smatch match;
    const std::string filename = entry.path().filename().string();
    if (!entry.is_directory() || !std::regex_match(filename, match, name))
      continue;
    const auto pid = static_cast<pid_t>(std::stoll(match[1]));
    // kill(pid, 0) probes for existence without sending anything; EPERM
    // means the process exists under another user.
    if (pid == ::getpid() || ::kill(pid, 0) == 0 || errno == EPERM)
      continue;
    std::error_code remove_ec;
    std::filesystem::remove_all(entry.path(), remove_ec);
    if (remove_ec) {
      spdlog::warn("Could not remove stale spill directory {}: {}",
                   entry.path().string(), remove_ec.message());
      continue;
    }
    spdlog::info("Removed stale spill directory {}", entry.path().string());
    ++removed;
  }
  return removed;
}

void log_spill_summary(const SpillDiagnostics &d) {
  spdlog::info("Peak Arrow memory {}; peak accounted Stage 2 memory {}",
               format_size(d.peak_arrow_bytes),
               format_size(d.peak_accounted_bytes));
  if (d.runs_written() == 0 && d.spill_events == 0)
    return;
  auto stream = [](const char *name, const SpillStreamStats &s) {
    spdlog::info("Spilled {}: {} runs, {}, {} merge passes", name,
                 s.runs_written, format_size(s.bytes_written), s.merge_passes);
  };
  stream("creations", d.creations);
  stream("receivers", d.receivers);
  stream("instance locations", d.locations);
  spdlog::info("{} spill events, {} written in total; largest receiver "
               "group {} records",
               d.spill_events, format_size(d.bytes_written()),
               d.largest_receiver_group);
  if (d.oversized_receiver_groups > 0)
    spdlog::warn("{} receiver groups exceeded the per-group budget and were "
                 "linked from disk",
                 d.oversized_receiver_groups);
}

auto SpillContext::directory() -> SpillDirectory & {
  if (!directory_)
    throw std::logic_error("spill requested without a spill directory");
  return *directory_;
}

void SpillContext::account_write(std::size_t bytes) {
  bytes_written_ += bytes;
  if (settings_.fail_writes_after_bytes > 0 &&
      bytes_written_ > settings_.fail_writes_after_bytes)
    throw std::runtime_error("Cannot write spill run: injected write failure");
}

RunWriter::RunWriter(std::filesystem::path path, uint32_t type_id,
                     SpillContext &context)
    : path_(std::move(path)), context_(context),
      buffer_(context.settings().io_buffer_bytes) {
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd_ < 0)
    fail("Cannot create spill run", path_, errno);
  put_bytes(kMagic.data(), kMagic.size());
  put(kFormatVersion);
  put(type_id);
  put(uint64_t{0}); // record count, patched by finish()
}

RunWriter::~RunWriter() {
  if (fd_ >= 0)
    ::close(fd_);
}

void RunWriter::put_bytes(const void *data, std::size_t size) {
  const auto *bytes = static_cast<const char *>(data);
  while (size > 0) {
    if (used_ == buffer_.size())
      flush();
    const std::size_t chunk = std::min(size, buffer_.size() - used_);
    std::memcpy(buffer_.data() + used_, bytes, chunk);
    used_ += chunk;
    bytes += chunk;
    size -= chunk;
  }
}

void RunWriter::flush() {
  if (used_ == 0)
    return;
  context_.account_write(used_);
  write_all(fd_, buffer_.data(), used_, path_);
  size_ += used_;
  used_ = 0;
}

auto RunWriter::finish() -> uint64_t {
  flush();
  if (::pwrite(fd_, &count_, sizeof count_, kCountOffset) !=
      static_cast<ssize_t>(sizeof count_))
    fail("Cannot write spill run header", path_, errno);
  if (::close(std::exchange(fd_, -1)) != 0)
    fail("Cannot close spill run", path_, errno);
  return size_;
}

RunReader::RunReader(std::filesystem::path path, uint32_t type_id,
                     std::size_t buffer_bytes, bool remove_at_end)
    : path_(std::move(path)), remove_at_end_(remove_at_end),
      buffer_(std::max<std::size_t>(buffer_bytes, kHeaderBytes)) {
  fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd_ < 0)
    fail("Cannot open spill run", path_, errno);
  try {
    std::array<char, 8> magic{};
    get_bytes(magic.data(), magic.size());
    const auto version = get<uint32_t>();
    const auto stored_type = get<uint32_t>();
    remaining_ = get<uint64_t>();
    if (magic != kMagic || version != kFormatVersion || stored_type != type_id)
      throw std::runtime_error("Not a spill run of the expected kind: " +
                               path_.string());
  } catch (...) {
    ::close(std::exchange(fd_, -1));
    throw;
  }
}

RunReader::~RunReader() { close(); }

void RunReader::close() {
  if (fd_ < 0)
    return;
  ::close(std::exchange(fd_, -1));
  if (remove_at_end_) {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
}

auto RunReader::fill() -> bool {
  for (;;) {
    const ssize_t got = ::read(fd_, buffer_.data(), buffer_.size());
    if (got < 0) {
      if (errno == EINTR)
        continue;
      fail("Cannot read spill run", path_, errno);
    }
    pos_ = 0;
    end_ = static_cast<std::size_t>(got);
    return got > 0;
  }
}

void RunReader::get_bytes(void *data, std::size_t size) {
  auto *bytes = static_cast<char *>(data);
  while (size > 0) {
    if (pos_ == end_ && !fill())
      throw std::runtime_error("Spill run is truncated: " + path_.string());
    const std::size_t chunk = std::min(size, end_ - pos_);
    std::memcpy(bytes, buffer_.data() + pos_, chunk);
    pos_ += chunk;
    bytes += chunk;
    size -= chunk;
  }
}

void RunReader::end_record() {
  if (remaining_ > 0)
    --remaining_;
  if (remaining_ > 0)
    return;
  if (pos_ != end_ || fill())
    throw std::runtime_error("Spill run has data past its last record: " +
                             path_.string());
  close();
}

} // namespace charmvz
