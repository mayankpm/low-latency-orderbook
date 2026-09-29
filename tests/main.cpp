// Runs every registered test, or only those whose name contains one of the
// command-line arguments (e.g. `obtests book.` or `obtests differential`).
#include <chrono>
#include <cstdio>
#include <exception>

#include "test.h"

int main(int argc, char** argv) {
  std::vector<std::string> filters(argv + 1, argv + argc);
  int passed = 0, failed = 0;
  std::vector<std::string> failures;
  for (const auto& t : obtest::Registry()) {
    if (!filters.empty()) {
      bool match = false;
      for (const auto& f : filters) match = match || t.name.find(f) != std::string::npos;
      if (!match) continue;
    }
    std::printf("[ RUN  ] %s\n", t.name.c_str());
    std::fflush(stdout);
    const auto start = std::chrono::steady_clock::now();
    try {
      t.fn();
      const auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      std::printf("[  OK  ] %s (%lld ms)\n", t.name.c_str(), static_cast<long long>(ms));
      passed++;
    } catch (const std::exception& e) {
      std::printf("[ FAIL ] %s\n         %s\n", t.name.c_str(), e.what());
      failures.push_back(t.name);
      failed++;
    }
    std::fflush(stdout);
  }
  std::printf("\n%d passed, %d failed\n", passed, failed);
  for (const auto& f : failures) std::printf("  FAILED: %s\n", f.c_str());
  return failed == 0 ? 0 : 1;
}
