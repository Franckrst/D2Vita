// src/crashreport/cr_io.h
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace d2cr {
struct DirEntry { std::string name; bool is_dir = false; uint64_t size = 0; int64_t mtime_unix = 0; };
class IoApi {
 public:
  virtual ~IoApi() = default;
  virtual bool read_file(const std::string& path, std::string* out, size_t max_bytes) = 0;
  virtual bool read_head_tail(const std::string& path, size_t head, size_t tail, std::string* out) = 0;
  virtual bool write_file(const std::string& path, const std::string& data) = 0;  // create/truncate
  virtual bool rename(const std::string& from, const std::string& to) = 0;        // Vita: sceIoRename
  virtual bool remove(const std::string& path) = 0;
  virtual bool mkdir(const std::string& path) = 0;
  virtual bool list_dir(const std::string& path, std::vector<DirEntry>* out) = 0;
  virtual bool stat(const std::string& path, DirEntry* out) = 0;
  // Stream: reads by chunks, callback invoked for each chunk; false = abort.
  virtual bool read_stream(const std::string& path, size_t chunk,
                           bool (*cb)(const uint8_t*, size_t, void*), void* ud) = 0;
};
std::unique_ptr<IoApi> make_posix_io();   // cr_io_posix.cpp (PC tests)
std::unique_ptr<IoApi> make_vita_io();    // cr_io_vita.cpp (console)
}  // namespace d2cr
