#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cachesim::internal {

// A forward-only stream of bytes with a peek/consume interface.
//
// The shape matters for speed. A read(buffer, n) interface forces a copy per
// call and a virtual call per record; peek(n) instead hands back a window
// into memory the source already owns, so a fixed-record reader can take a
// megabyte at a time and iterate it with no calls and no copies at all. For
// a memory-mapped file the window is the file itself, and parsing a binary
// trace becomes a loop over mapped pages — which is as close to free as
// trace ingestion gets.
class IByteSource {
 public:
  virtual ~IByteSource() = default;

  // Returns a window of up to `wanted` bytes starting at the current
  // position, refilling if needed. A shorter span means end of stream is
  // within reach; an empty span means it has been reached. The span stays
  // valid until the next peek() or consume().
  virtual std::span<const std::byte> peek(std::size_t wanted) = 0;

  // Advances past `count` bytes, which must not exceed the last peek's size.
  virtual void consume(std::size_t count) = 0;

  // Back to the beginning. Used when a trace is replayed more than once.
  virtual void reset() = 0;

  // Total bytes in the stream if known, 0 if not (a compressed or piped
  // stream does not know). Used only to pre-size buffers and to estimate
  // progress.
  [[nodiscard]] virtual std::uint64_t totalBytes() const = 0;

  // Bytes consumed so far.
  [[nodiscard]] virtual std::uint64_t position() const = 0;

  [[nodiscard]] virtual const std::string& name() const = 0;
};

// A read-only memory map over a whole file.
//
// The default for any uncompressed trace, and the reason a parameter sweep
// here does not need libCacheSim's shared reader thread and batch barrier:
// every worker maps the same file independently, the kernel backs all those
// mappings with one set of page-cache pages, and no worker ever waits for
// another. The trace is read from disk once no matter how many
// configurations are running.
class MmapSource final : public IByteSource {
 public:
  explicit MmapSource(std::string path);
  ~MmapSource() override;

  MmapSource(const MmapSource&) = delete;
  MmapSource& operator=(const MmapSource&) = delete;

  std::span<const std::byte> peek(std::size_t wanted) override;
  void consume(std::size_t count) override;
  void reset() override { position_ = 0; }
  [[nodiscard]] std::uint64_t totalBytes() const override { return size_; }
  [[nodiscard]] std::uint64_t position() const override { return position_; }
  [[nodiscard]] const std::string& name() const override { return path_; }

  // The whole mapping. Lets a reader that needs to look at a header, or to
  // index records by position, do so without going through peek/consume.
  [[nodiscard]] std::span<const std::byte> all() const {
    if (data_ == nullptr) return {};
    return {static_cast<const std::byte*>(data_), size_};
  }

 private:
  std::string path_;
  void* data_ = nullptr;
  std::size_t size_ = 0;
  std::uint64_t position_ = 0;
};

// Buffered reads from a file descriptor. Used when the input cannot be
// mapped — a pipe, a character device, or a file that grew since it was
// opened.
class FileSource final : public IByteSource {
 public:
  explicit FileSource(std::string path, std::size_t buffer_bytes = 1u << 20);
  ~FileSource() override;

  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;

  std::span<const std::byte> peek(std::size_t wanted) override;
  void consume(std::size_t count) override;
  void reset() override;
  [[nodiscard]] std::uint64_t totalBytes() const override { return size_; }
  [[nodiscard]] std::uint64_t position() const override { return position_; }
  [[nodiscard]] const std::string& name() const override { return path_; }

 private:
  void fill(std::size_t wanted);

  std::string path_;
  int fd_ = -1;
  std::vector<std::byte> buffer_;
  std::size_t begin_ = 0;  // first unconsumed byte in buffer_
  std::size_t end_ = 0;    // one past the last valid byte in buffer_
  std::uint64_t size_ = 0;
  std::uint64_t position_ = 0;
  bool eof_ = false;
};

// Streaming zstd decompression over another source.
//
// Trace files are routinely shipped compressed — this user's own merged
// memory traces are .zstd — and decompressing them to disk first can mean
// tens of times the storage. Streaming costs a memcpy per block, which is
// still far cheaper than the alternative, and the decompressed window
// presented upstream looks exactly like a mapped file.
//
// Multi-frame files are handled: zstd's own CLI concatenates frames when
// appending, so a trace built incrementally is a sequence of frames rather
// than one.
class ZstdSource final : public IByteSource {
 public:
  explicit ZstdSource(std::unique_ptr<IByteSource> compressed,
                      std::size_t buffer_bytes = 1u << 22);
  ~ZstdSource() override;

  ZstdSource(const ZstdSource&) = delete;
  ZstdSource& operator=(const ZstdSource&) = delete;

  std::span<const std::byte> peek(std::size_t wanted) override;
  void consume(std::size_t count) override;
  void reset() override;
  // Unknowable without decompressing the whole stream.
  [[nodiscard]] std::uint64_t totalBytes() const override { return 0; }
  [[nodiscard]] std::uint64_t position() const override { return position_; }
  [[nodiscard]] const std::string& name() const override { return source_->name(); }

  // Compressed bytes consumed out of the underlying source, and its total.
  // Progress reporting uses these, since the decompressed length is unknown.
  [[nodiscard]] std::uint64_t compressedPosition() const { return source_->position(); }
  [[nodiscard]] std::uint64_t compressedTotal() const { return source_->totalBytes(); }

  // Whether this build has zstd support at all.
  [[nodiscard]] static bool available();

 private:
  void decompressMore(std::size_t wanted);

  std::unique_ptr<IByteSource> source_;
  std::vector<std::byte> buffer_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
  std::uint64_t position_ = 0;
  bool eof_ = false;
  // Opaque ZSTD_DStream*, so that zstd.h stays out of this header and callers
  // do not inherit the dependency.
  void* stream_ = nullptr;
};

// Opens `path` as a byte source, memory-mapping it when possible and
// transparently wrapping it in zstd decompression when the file starts with
// the zstd magic number (0xFD2FB528) or ends in .zst/.zstd. Throws
// TraceError if the file cannot be opened, or if it is compressed and this
// build has no zstd support.
std::unique_ptr<IByteSource> openByteSource(const std::string& path);

// Whether the file looks zstd-compressed, by magic number first and
// extension second.
[[nodiscard]] bool looksZstdCompressed(const std::string& path);

}  // namespace cachesim::internal
