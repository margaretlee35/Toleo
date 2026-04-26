#include "cxl_mem_sim.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace cxlmem;

static std::uint64_t read_u64(const char *v) {
  if (!v) throw std::runtime_error("missing option value");
  return static_cast<std::uint64_t>(std::strtoull(v, nullptr, 10));
}

int main(int argc, char **argv) {
  SimConfig cfg;

  for (int i = 1; i < argc; ++i) {
    std::string a(argv[i]);
    auto next = [&]() -> std::uint64_t { return (i + 1 < argc) ? read_u64(argv[++i]) : 0; };

    if (a == "--read-percent") cfg.read_percent = static_cast<int>(next());
    else if (a == "--num-reqs") cfg.num_reqs = next();
    else if (a == "--link-latency") cfg.link_latency = next();
    else if (a == "--serdes-time-per-flit") cfg.serdes_time_per_flit = next();
    else if (a == "--backend-read-latency") cfg.backend_read_latency = next();
    else if (a == "--backend-write-latency") cfg.backend_write_latency = next();
    else if (a == "--max-outstanding") cfg.max_outstanding = static_cast<std::uint32_t>(next());
    else if (a == "--help") {
      std::cout << "Usage: ./cxl_mem_sim [options]\n"
                << "  --read-percent N (0-100, default 50)\n"
                << "  --num-reqs N (default 400)\n"
                << "  --link-latency N (ns, default 100)\n"
                << "  --serdes-time-per-flit N (ns, default 2)\n"
                << "  --backend-read-latency N (ns, default 50)\n"
                << "  --backend-write-latency N (ns, default 30)\n"
                << "  --max-outstanding N (default 16)\n";
      return 0;
    } else {
      std::cerr << "Unknown option: " << a << "\n";
      return 2;
    }
  }

  if (cfg.read_percent < 0 || cfg.read_percent > 100) {
    std::cerr << "--read-percent must be in [0, 100]\n";
    return 2;
  }

  CxlMemSimulator sim(cfg);
  SimStats st = sim.run();

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "avg_read_latency_ns: " << st.avg_read_latency_ns << "\n";
  std::cout << "avg_write_latency_ns: " << st.avg_write_latency_ns << "\n";
  std::cout << "throughput_reqs_per_ns: " << st.throughput_reqs_per_ns << "\n";
  std::cout << "avg_flit_utilization_pct: " << st.avg_flit_utilization_pct << "\n";
  std::cout << "avg_bytes_wasted_per_flit: " << st.avg_bytes_wasted_per_flit << "\n";
  std::cout << "total_m2s_flits: " << st.total_m2s_flits << "\n";
  std::cout << "total_s2m_flits: " << st.total_s2m_flits << "\n";
  std::cout << "total_data_flits: " << st.total_data_flits << "\n";
  std::cout << "total_header_flits: " << st.total_header_flits << "\n";
  return 0;
}
