#include "cxl_mem_sim.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace cxlmem;

static uint64_t readU64(const char *arg, uint64_t dflt) {
  return arg ? static_cast<uint64_t>(std::strtoull(arg, nullptr, 10)) : dflt;
}

int main(int argc, char **argv) {
  CxlConfig cfg;
  uint64_t num_reqs = 200;
  uint32_t read_percent = 50;
  uint32_t req_size = 64;
  uint64_t stride = 64;

  for (int i = 1; i < argc; ++i) {
    std::string a(argv[i]);
    auto next = [&](uint64_t dflt) { return (i + 1 < argc) ? readU64(argv[++i], dflt) : dflt; };

    if (a == "--num-reqs") num_reqs = next(num_reqs);
    else if (a == "--read-percent") read_percent = static_cast<uint32_t>(next(read_percent));
    else if (a == "--req-size") req_size = static_cast<uint32_t>(next(req_size));
    else if (a == "--stride") stride = next(stride);
    else if (a == "--base-m2s-write-slots") cfg.base_m2s_write_slots = static_cast<uint32_t>(next(cfg.base_m2s_write_slots));
    else if (a == "--base-s2m-read-rsp-slots") cfg.base_s2m_read_rsp_slots = static_cast<uint32_t>(next(cfg.base_s2m_read_rsp_slots));
    else if (a == "--extra-m2s-write") cfg.extra_slot_enable_m2s_write = (next(1) != 0);
    else if (a == "--extra-s2m-read-rsp") cfg.extra_slot_enable_s2m_read_rsp = (next(1) != 0);
    else if (a == "--packetize-delay") cfg.packetize_delay = next(cfg.packetize_delay);
    else if (a == "--depacketize-delay") cfg.depacketize_delay = next(cfg.depacketize_delay);
    else if (a == "--serdes-time-per-flit") cfg.serdes_time_per_flit = next(cfg.serdes_time_per_flit);
    else if (a == "--link-latency") cfg.link_latency = next(cfg.link_latency);
    else if (a == "--backend-read-latency") cfg.backend_read_latency = next(cfg.backend_read_latency);
    else if (a == "--backend-write-latency") cfg.backend_write_latency = next(cfg.backend_write_latency);
    else if (a == "--max-outstanding") cfg.max_outstanding = static_cast<uint32_t>(next(cfg.max_outstanding));
    else if (a == "--flush-policy") {
      uint64_t mode = next(0);
      cfg.flush_policy = mode ? FlushPolicy::ALWAYS : FlushPolicy::ON_DEMAND;
    } else if (a == "--debug") cfg.debug = (next(1) != 0);
    else if (a == "--help") {
      std::cout << "cxl_mem_sim options:\n"
                << "  --num-reqs N\n"
                << "  --read-percent N\n"
                << "  --base-m2s-write-slots N\n"
                << "  --base-s2m-read-rsp-slots N\n"
                << "  --extra-m2s-write 0|1\n"
                << "  --extra-s2m-read-rsp 0|1\n"
                << "  --flush-policy 0(on_demand)|1(always)\n";
      return 0;
    }
  }

  EventLoop ev;
  SimStats stats;

  CxlLinkModel link(ev, cfg, stats);
  DramBackend dram(ev, cfg);
  CxlHostAdapter host(ev, cfg, link, stats);
  CxlDeviceAdapter dev(ev, cfg, link, dram, stats);
  host.setDevice(&dev);
  dev.setHost(&host);

  SimpleCpuDriver cpu(ev, cfg, host, stats);
  cpu.runSynthetic(num_reqs, read_percent, req_size, stride);

  ev.run();

  cpu.printSummary();

  std::cout << "total_m2s_flits=" << stats.total_m2s_flits_sent << "\n";
  std::cout << "total_s2m_flits=" << stats.total_s2m_flits_sent << "\n";
  std::cout << "total_slots_m2s_rd_req=" << stats.total_slots_m2s_rd_req << "\n";
  std::cout << "total_slots_m2s_wr_req=" << stats.total_slots_m2s_wr_req << "\n";
  std::cout << "total_slots_s2m_drsp=" << stats.total_slots_s2m_drsp << "\n";
  std::cout << "total_slots_s2m_ndr=" << stats.total_slots_s2m_ndr << "\n";
  std::cout << "total_extra_slots=" << stats.total_extra_slots << "\n";
  std::cout << "m2s_queue_delay=" << stats.m2s_queue_delay << "\n";
  std::cout << "s2m_queue_delay=" << stats.s2m_queue_delay << "\n";
  std::cout << "requests_with_changed_flit_count="
            << stats.requests_with_changed_flit_count << "\n";

  double sim_time = static_cast<double>(ev.now() ? ev.now() : 1);
  double throughput = static_cast<double>(stats.read_count + stats.write_count) / sim_time;
  std::cout << "throughput_reqs_per_cycle=" << throughput << "\n";
  return 0;
}
