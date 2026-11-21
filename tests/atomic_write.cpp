// Linux: c++ -std=c++17 -Isrc/include tests/atomic_write.cpp -Wl,--wrap=fsync -o /tmp/test-atomic && /tmp/test-atomic
#include "zenoh_bridge/protocol.hpp"

#include <cassert>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;
namespace {
std::string path;
std::vector<std::string> observations;
int fail_at = 0;

std::string current() {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), {}};
}
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  struct stat st{};
  assert(::fstat(fd, &st) == 0);
  observations.push_back(std::string(S_ISDIR(st.st_mode) ? "dir:" : "file:") + current());
  if (static_cast<int>(observations.size()) == fail_at) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

int main() {
  char dir[] = "/tmp/zbridge-atomic-XXXXXX";
  if (!::mkdtemp(dir)) return 1;
  path = std::string(dir) + "/settings";
  for (int fault : {0, 1, 2}) {
    std::ofstream(path) << "old";
    observations.clear();
    fail_at = fault;
    bool threw = false;
    try { zenoh_bridge::atomicWriteFile(path, "new"); }
    catch (const std::runtime_error &) { threw = true; }
    assert(threw == (fault != 0));
    assert(observations.front() == "file:old");
    assert(observations.size() == (fault == 1 ? 1u : 2u));
    if (fault != 1) assert(observations.back() == "dir:new");
    assert(current() == (fault == 1 ? "old" : "new"));
    assert(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 1);
  }
  fs::remove_all(dir);
}
