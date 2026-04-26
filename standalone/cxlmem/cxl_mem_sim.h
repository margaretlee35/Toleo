#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <queue>
#include <string>
#include <vector>

namespace cxlmem {

using SimTime = std::uint64_t;

struct SimConfig {
  int read_percent = 50;
  std::uint64_t num_reqs = 400;
  SimTime link_latency = 100;
  SimTime serdes_time_per_flit = 2;
  SimTime backend_read_latency = 50;
  SimTime backend_write_latency = 30;
  std::uint32_t max_outstanding = 16;
};

struct SimStats {
  double avg_read_latency_ns = 0.0;
  double avg_write_latency_ns = 0.0;
  double throughput_reqs_per_ns = 0.0;
  double avg_flit_utilization_pct = 0.0;
  double avg_bytes_wasted_per_flit = 0.0;

  std::uint64_t total_m2s_flits = 0;
  std::uint64_t total_s2m_flits = 0;
  std::uint64_t total_data_flits = 0;
  std::uint64_t total_header_flits = 0;
};

class CxlMemSimulator {
 public:
  explicit CxlMemSimulator(const SimConfig &cfg);
  SimStats run();

 private:
  enum class ReqType { READ, WRITE };
  enum class LinkDir { M2S, S2M };
  enum class MsgType { M2S_REQ, M2S_RWD, S2M_DRS, S2M_NDR };
  enum class FlitType { H4, H5, G0, G4, G5, G6 };

  struct Event {
    SimTime time = 0;
    std::uint64_t seq = 0;
    std::function<void()> fn;
    bool operator<(const Event &other) const {
      if (time != other.time) return time > other.time;
      return seq > other.seq;
    }
  };

  struct Request {
    std::uint64_t id = 0;
    ReqType type = ReqType::READ;
    SimTime issued_at = 0;
  };

  struct LinkMessage {
    MsgType type;
    std::uint64_t req_id;
  };

  struct PackedFlit {
    LinkDir dir;
    FlitType type;
    std::uint32_t bytes_used_in_payload = 0;
    std::uint32_t bytes_wasted = 0;
    std::vector<std::uint64_t> complete_on_arrival;
  };

  void schedule(SimTime when, std::function<void()> fn);
  void process_events();
  void try_issue_requests();

  void enqueue_m2s(MsgType type, std::uint64_t req_id);
  void enqueue_s2m(MsgType type, std::uint64_t req_id);
  void flush_m2s();
  void flush_s2m();

  PackedFlit build_m2s_h5();
  PackedFlit build_m2s_h4_and_pack_reqs();
  PackedFlit build_s2m_g4_or_g6(std::vector<PackedFlit> &extra_data_flits);
  PackedFlit build_s2m_g5();
  PackedFlit build_g0(LinkDir dir, std::uint64_t req_id);

  void send_flit(PackedFlit flit, SimTime now);
  void on_completion(std::uint64_t req_id, SimTime done_at);

  static constexpr std::uint32_t kPayloadBytes = 64;
  static constexpr std::uint32_t kHeaderBytes = 4;

  SimConfig cfg_;
  std::priority_queue<Event> events_;
  std::uint64_t event_seq_ = 0;
  SimTime now_ = 0;

  std::deque<LinkMessage> m2s_q_;
  std::deque<LinkMessage> s2m_q_;
  SimTime next_m2s_ready_ = 0;
  SimTime next_s2m_ready_ = 0;

  std::uint64_t next_req_id_ = 1;
  std::uint64_t issued_ = 0;
  std::uint64_t completed_ = 0;
  std::uint64_t outstanding_ = 0;

  std::vector<Request> requests_;

  std::uint64_t read_count_ = 0;
  std::uint64_t write_count_ = 0;
  long double read_latency_sum_ = 0.0;
  long double write_latency_sum_ = 0.0;

  std::uint64_t total_flits_ = 0;
  long double utilized_bytes_sum_ = 0.0;
  long double wasted_bytes_sum_ = 0.0;
  long double queue_delay_m2s_ = 0.0;
  long double queue_delay_s2m_ = 0.0;

  SimTime last_completion_time_ = 0;

  std::uint64_t m2s_flits_ = 0;
  std::uint64_t s2m_flits_ = 0;
  std::uint64_t data_flits_ = 0;
  std::uint64_t header_flits_ = 0;
};

}  // namespace cxlmem
