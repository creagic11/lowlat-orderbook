#pragma once
// Reading Nasdaq ITCH sample files.
//
// The sample files (emi.nasdaq.com/ITCH/) are a sequence of
//   [2-byte big-endian length][message of that length]
// with no other framing. The file is mapped read-only and messages are handed
// out as pointers into the mapping: zero copies.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "lob/endian.hpp"

namespace lob::itch {

class MappedFile {
   public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("cannot open " + path);
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error("cannot stat " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ > 0) {
            void* m = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (m == MAP_FAILED) {
                ::close(fd_);
                throw std::runtime_error("cannot mmap " + path);
            }
            data_ = static_cast<const std::uint8_t*>(m);
            ::madvise(m, size_, MADV_SEQUENTIAL);
        }
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept
        : fd_(std::exchange(o.fd_, -1)),
          data_(std::exchange(o.data_, nullptr)),
          size_(std::exchange(o.size_, 0)) {}
    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

   private:
    int fd_ = -1;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

struct FramingResult {
    std::size_t messages = 0;
    std::size_t bytes_consumed = 0;
    bool truncated = false;  // file ended inside a length prefix or a message
};

// Calls f(const uint8_t* msg, std::size_t len) for every length-prefixed message.
template <class F>
FramingResult for_each_message(const std::uint8_t* data, std::size_t size, F&& f) {
    FramingResult r;
    std::size_t off = 0;
    while (off + 2 <= size) {
        const std::size_t len = load_be16(data + off);
        if (off + 2 + len > size) {
            r.truncated = true;
            break;
        }
        f(data + off + 2, len);
        off += 2 + len;
        ++r.messages;
    }
    if (off < size && !r.truncated) r.truncated = true;
    r.bytes_consumed = off;
    return r;
}

}  // namespace lob::itch
