#include "internal/trace/byte_source.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "internal/common/error.hpp"
#include "internal/common/string_util.hpp"

#if CACHESIM_HAVE_ZSTD
#include <zstd.h>
#endif

namespace cachesim::internal {
namespace {

std::string errnoMessage(const char* what, const std::string& path) {
  return std::string(what) + " '" + path + "': " + std::strerror(errno);
}

}  // namespace

// ---------------------------------------------------------------- MmapSource

MmapSource::MmapSource(std::string path) : path_(std::move(path)) {
  const int fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) throw TraceError(errnoMessage("cannot open", path_));

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const std::string message = errnoMessage("cannot stat", path_);
    ::close(fd);
    throw TraceError(message);
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    throw TraceError("'" + path_ + "' is not a regular file, so it cannot be mapped");
  }
  size_ = static_cast<std::size_t>(st.st_size);

  if (size_ > 0) {
    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      const std::string message = errnoMessage("cannot mmap", path_);
      ::close(fd);
      throw TraceError(message);
    }
    // The access pattern is a single forward pass, every byte read once.
    // SEQUENTIAL tells the kernel to read ahead aggressively and to drop
    // pages behind us, which keeps a 100 GB trace from evicting everything
    // else on the machine from the page cache.
    ::madvise(data_, size_, MADV_SEQUENTIAL);
    ::madvise(data_, size_, MADV_WILLNEED);
  }
  // The mapping outlives the descriptor, so there is no reason to keep it.
  ::close(fd);
}

MmapSource::~MmapSource() {
  if (data_ != nullptr) ::munmap(data_, size_);
}

std::span<const std::byte> MmapSource::peek(std::size_t wanted) {
  if (data_ == nullptr) return {};
  const std::size_t available = size_ - static_cast<std::size_t>(position_);
  return {static_cast<const std::byte*>(data_) + position_, std::min(wanted, available)};
}

void MmapSource::consume(std::size_t count) {
  position_ += count;
  if (position_ > size_) position_ = size_;
}

// ---------------------------------------------------------------- FileSource

FileSource::FileSource(std::string path, std::size_t buffer_bytes) : path_(std::move(path)) {
  fd_ = ::open(path_.c_str(), O_RDONLY);
  if (fd_ < 0) throw TraceError(errnoMessage("cannot open", path_));
  struct stat st {};
  if (::fstat(fd_, &st) == 0 && S_ISREG(st.st_mode)) {
    size_ = static_cast<std::uint64_t>(st.st_size);
  }
  buffer_.resize(std::max<std::size_t>(buffer_bytes, 1u << 16));
}

FileSource::~FileSource() {
  if (fd_ >= 0) ::close(fd_);
}

void FileSource::fill(std::size_t wanted) {
  // Make room: slide whatever is unconsumed to the front, and grow the buffer
  // if a single record is bigger than it.
  if (begin_ > 0) {
    const std::size_t live = end_ - begin_;
    if (live > 0) std::memmove(buffer_.data(), buffer_.data() + begin_, live);
    begin_ = 0;
    end_ = live;
  }
  if (wanted > buffer_.size()) buffer_.resize(wanted * 2);

  while (end_ - begin_ < wanted && !eof_) {
    const ssize_t got = ::read(fd_, buffer_.data() + end_, buffer_.size() - end_);
    if (got < 0) {
      if (errno == EINTR) continue;
      throw TraceError(errnoMessage("read failed on", path_));
    }
    if (got == 0) {
      eof_ = true;
      break;
    }
    end_ += static_cast<std::size_t>(got);
  }
}

std::span<const std::byte> FileSource::peek(std::size_t wanted) {
  if (end_ - begin_ < wanted && !eof_) fill(wanted);
  return {buffer_.data() + begin_, std::min(wanted, end_ - begin_)};
}

void FileSource::consume(std::size_t count) {
  const std::size_t live = end_ - begin_;
  const std::size_t step = std::min(count, live);
  begin_ += step;
  position_ += step;
}

void FileSource::reset() {
  if (::lseek(fd_, 0, SEEK_SET) == static_cast<off_t>(-1)) {
    throw TraceError(errnoMessage("cannot rewind", path_));
  }
  begin_ = 0;
  end_ = 0;
  position_ = 0;
  eof_ = false;
}

// ---------------------------------------------------------------- ZstdSource

bool ZstdSource::available() {
#if CACHESIM_HAVE_ZSTD
  return true;
#else
  return false;
#endif
}

#if CACHESIM_HAVE_ZSTD

ZstdSource::ZstdSource(std::unique_ptr<IByteSource> compressed, std::size_t buffer_bytes)
    : source_(std::move(compressed)) {
  buffer_.resize(std::max<std::size_t>(buffer_bytes, ZSTD_DStreamOutSize()));
  stream_ = ZSTD_createDStream();
  if (stream_ == nullptr) throw TraceError("cannot create a zstd decompression stream");
  ZSTD_initDStream(static_cast<ZSTD_DStream*>(stream_));
}

ZstdSource::~ZstdSource() {
  if (stream_ != nullptr) ZSTD_freeDStream(static_cast<ZSTD_DStream*>(stream_));
}

void ZstdSource::decompressMore(std::size_t wanted) {
  // Slide the unconsumed tail to the front, and grow if one record is bigger
  // than the whole buffer.
  if (begin_ > 0) {
    const std::size_t live = end_ - begin_;
    if (live > 0) std::memmove(buffer_.data(), buffer_.data() + begin_, live);
    begin_ = 0;
    end_ = live;
  }
  if (wanted > buffer_.size()) buffer_.resize(wanted * 2);

  auto* stream = static_cast<ZSTD_DStream*>(stream_);
  while (end_ - begin_ < wanted && !eof_) {
    // A big compressed window keeps the number of upstream peeks down; the
    // span is a view into the mapped file, so there is no copy here.
    const std::span<const std::byte> input = source_->peek(ZSTD_DStreamInSize());
    if (input.empty()) {
      eof_ = true;
      break;
    }
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    ZSTD_outBuffer out{buffer_.data() + end_, buffer_.size() - end_, 0};
    const std::size_t result = ZSTD_decompressStream(stream, &out, &in);
    if (ZSTD_isError(result)) {
      throw TraceError("zstd decompression failed on '" + source_->name() + "': " +
                       ZSTD_getErrorName(result));
    }
    source_->consume(in.pos);
    end_ += out.pos;
    if (result == 0) {
      // End of a frame. zstd concatenates frames when a file is appended to,
      // so re-initialize and keep going rather than stopping here; a stream
      // that really has ended will give an empty input next time round.
      ZSTD_initDStream(stream);
    }
    if (in.pos == 0 && out.pos == 0) {
      // No forward progress with input still available means the stream is
      // truncated or corrupt. Stop rather than spin.
      eof_ = true;
      break;
    }
  }
}

void ZstdSource::reset() {
  source_->reset();
  ZSTD_initDStream(static_cast<ZSTD_DStream*>(stream_));
  begin_ = 0;
  end_ = 0;
  position_ = 0;
  eof_ = false;
}

#else  // !CACHESIM_HAVE_ZSTD

ZstdSource::ZstdSource(std::unique_ptr<IByteSource> compressed, std::size_t buffer_bytes)
    : source_(std::move(compressed)) {
  (void)buffer_bytes;
  throw TraceError(
      "this build has no zstd support, so compressed trace '" + source_->name() +
      "' cannot be read. Install libzstd development headers and rebuild, or "
      "decompress the trace first (zstd -d).");
}

ZstdSource::~ZstdSource() = default;

void ZstdSource::decompressMore(std::size_t wanted) { (void)wanted; }

void ZstdSource::reset() {}

#endif

std::span<const std::byte> ZstdSource::peek(std::size_t wanted) {
  if (end_ - begin_ < wanted && !eof_) decompressMore(wanted);
  return {buffer_.data() + begin_, std::min(wanted, end_ - begin_)};
}

void ZstdSource::consume(std::size_t count) {
  const std::size_t step = std::min(count, end_ - begin_);
  begin_ += step;
  position_ += step;
}

// ---------------------------------------------------------------- factory

bool looksZstdCompressed(const std::string& path) {
  // Magic number first, because a trace's extension says what someone named
  // it and the magic says what it is.
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    unsigned char magic[4] = {0, 0, 0, 0};
    const ssize_t got = ::read(fd, magic, sizeof(magic));
    ::close(fd);
    if (got == 4) {
      // 0xFD2FB528, little-endian on disk.
      if (magic[0] == 0x28 && magic[1] == 0xb5 && magic[2] == 0x2f && magic[3] == 0xfd) {
        return true;
      }
      // Skippable frames (0x184D2A50..0x184D2A5F) also start a zstd stream.
      if (magic[0] >= 0x50 && magic[0] <= 0x5f && magic[1] == 0x2a && magic[2] == 0x4d &&
          magic[3] == 0x18) {
        return true;
      }
      return false;
    }
  }
  const std::string lowered = toLower(path);
  return lowered.size() > 4 &&
         (lowered.compare(lowered.size() - 4, 4, ".zst") == 0 ||
          (lowered.size() > 5 && lowered.compare(lowered.size() - 5, 5, ".zstd") == 0));
}

std::unique_ptr<IByteSource> openByteSource(const std::string& path) {
  const bool compressed = looksZstdCompressed(path);
  std::unique_ptr<IByteSource> raw;
  try {
    raw = std::make_unique<MmapSource>(path);
  } catch (const TraceError&) {
    // Not mappable (a pipe, a /proc file, a zero-length special file); fall
    // back to buffered reads, which work on anything readable.
    raw = std::make_unique<FileSource>(path);
  }
  if (!compressed) return raw;
  return std::make_unique<ZstdSource>(std::move(raw));
}

}  // namespace cachesim::internal
