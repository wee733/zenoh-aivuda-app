// Linux: c++ -std=c++17 -Isrc/include tests/atomic_write.cpp -Wl,--wrap=fsync -o /tmp/test-atomic && /tmp/test-atomic
#include "zenoh_bridge/config_transaction.hpp"

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
  const zenoh_bridge::ConfigFiles paths{{"bridge_settings", path}, {"routes", std::string(dir) + "/routes"}};
  const auto journal = path + ".import-journal";
  // Both fsyncs for the journal, each atomic file replacement, and the
  // directory sync after unlinking the journal must have consistent rollback.
  for (int fault = 0; fault <= 7; ++fault) {
    std::ofstream(path) << "old settings";
    std::ofstream(paths.at("routes")) << "old routes";
    observations.clear();
    fail_at = fault;
    bool threw = false;
    try {
      zenoh_bridge::commitConfigFiles(paths, {{"bridge_settings", "new settings"}, {"routes", "new routes"}});
    } catch (const std::runtime_error &) { threw = true; }
    assert(threw == (fault != 0));
    assert(!fs::exists(journal));
    assert(current() == (fault == 0 ? "new settings" : "old settings"));
    std::ifstream routes(paths.at("routes"));
    std::string contents{std::istreambuf_iterator<char>(routes), {}};
    assert(contents == (fault == 0 ? "new routes" : "old routes"));
  }
  fs::remove_all(dir);
}
