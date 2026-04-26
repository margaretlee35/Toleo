#include "cxl_mem_sim.h"

#include <algorithm>
#include <deque>
#include <random>

namespace cxlmem {

CxlMemSimulator::CxlMemSimulator(const SimConfig &cfg) : cfg_(cfg) {
  requests_.resize(cfg_.num_reqs + 1);
}

void CxlMemSimulator::schedule(SimTime when, std::function<void()> fn) {
  events_.push(Event{when, event_seq_++, std::move(fn)});
}

void CxlMemSimulator::process_events() {
  while (!events_.empty()) {
    Event ev = events_.top();
    events_.pop();
    now_ = ev.time;
    ev.fn();
  }
}

void CxlMemSimulator::try_issue_requests() {
  static std::mt19937_64 rng(0xC0FFEEULL);
  std::uniform_int_distribution<int> dist(1, 100);

  while (issued_ < cfg_.num_reqs && outstanding_ < cfg_.max_outstanding) {
    std::uint64_t id = next_req_id_++;
    bool is_read = dist(rng) <= cfg_.read_percent;

    Request req{id, is_read ? ReqType::READ : ReqType::WRITE, now_};
    requests_[id] = req;
    ++issued_;
    ++outstanding_;

    if (is_read) {
      enqueue_m2s(MsgType::M2S_REQ, id);
      schedule(now_ + cfg_.backend_read_latency, [this, id]() { enqueue_s2m(MsgType::S2M_DRS, id); });
    } else {
      enqueue_m2s(MsgType::M2S_RWD, id);
      schedule(now_ + cfg_.backend_write_latency, [this, id]() { enqueue_s2m(MsgType::S2M_NDR, id); });
    }
  }
}

void CxlMemSimulator::enqueue_m2s(MsgType type, std::uint64_t req_id) {
  m2s_q_.push_back(LinkMessage{type, req_id});
  flush_m2s();
}

void CxlMemSimulator::enqueue_s2m(MsgType type, std::uint64_t req_id) {
  s2m_q_.push_back(LinkMessage{type, req_id});
  flush_s2m();
}

CxlMemSimulator::PackedFlit CxlMemSimulator::build_m2s_h5() {
  m2s_q_.pop_front();
  std::uint32_t packed_extra = 0;
  std::uint32_t max_extra = (kPayloadBytes - kHeaderBytes) / kHeaderBytes;
  while (!m2s_q_.empty() && m2s_q_.front().type == MsgType::M2S_REQ && packed_extra < max_extra) {
    m2s_q_.pop_front();
    ++packed_extra;
  }
  std::uint32_t used = kHeaderBytes + packed_extra * kHeaderBytes;
  return PackedFlit{LinkDir::M2S, FlitType::H5, used, kPayloadBytes - used, {}};
}

CxlMemSimulator::PackedFlit CxlMemSimulator::build_m2s_h4_and_pack_reqs() {
  m2s_q_.pop_front();
  std::uint32_t packed_extra = 0;
  std::uint32_t max_extra = (kPayloadBytes - kHeaderBytes) / kHeaderBytes;
  while (!m2s_q_.empty() && m2s_q_.front().type == MsgType::M2S_REQ && packed_extra < max_extra) {
    m2s_q_.pop_front();
    ++packed_extra;
  }
  std::uint32_t used = kHeaderBytes + packed_extra * kHeaderBytes;
  return PackedFlit{LinkDir::M2S, FlitType::H4, used, kPayloadBytes - used, {}};
}

CxlMemSimulator::PackedFlit CxlMemSimulator::build_g0(LinkDir dir, std::uint64_t req_id) {
  std::vector<std::uint64_t> complete = req_id ? std::vector<std::uint64_t>{req_id} : std::vector<std::uint64_t>{};
  return PackedFlit{dir, FlitType::G0, kPayloadBytes - 2, 2, complete};
}

CxlMemSimulator::PackedFlit CxlMemSimulator::build_s2m_g4_or_g6(std::vector<PackedFlit> &extra_data_flits) {
  std::uint64_t drs_req = s2m_q_.front().req_id;

  bool has_ndr = false;
  for (std::size_t i = 1; i < s2m_q_.size(); ++i) {
    if (s2m_q_[i].type == MsgType::S2M_NDR) {
      has_ndr = true;
      break;
    }
  }

  if (has_ndr) {
    s2m_q_.pop_front();
    std::vector<std::uint64_t> packed_ndr;
    std::deque<LinkMessage> rem;
    for (const auto &m : s2m_q_) {
      if (m.type == MsgType::S2M_NDR && packed_ndr.size() < 2) {
        packed_ndr.push_back(m.req_id);
      } else {
        rem.push_back(m);
      }
    }
    s2m_q_.swap(rem);

    std::uint32_t used = kHeaderBytes + static_cast<std::uint32_t>(packed_ndr.size()) * kHeaderBytes;
    extra_data_flits.push_back(build_g0(LinkDir::S2M, drs_req));
    return PackedFlit{LinkDir::S2M, FlitType::G4, used, kPayloadBytes - used, packed_ndr};
  }

  std::vector<std::uint64_t> drs_ids;
  while (!s2m_q_.empty() && s2m_q_.front().type == MsgType::S2M_DRS && drs_ids.size() < 3) {
    drs_ids.push_back(s2m_q_.front().req_id);
    s2m_q_.pop_front();
  }
  std::uint32_t used = static_cast<std::uint32_t>(drs_ids.size()) * kHeaderBytes;
  for (std::uint64_t id : drs_ids) extra_data_flits.push_back(build_g0(LinkDir::S2M, id));
  return PackedFlit{LinkDir::S2M, FlitType::G6, used, kPayloadBytes - used, {}};
}

CxlMemSimulator::PackedFlit CxlMemSimulator::build_s2m_g5() {
  std::vector<std::uint64_t> ids;
  while (!s2m_q_.empty() && s2m_q_.front().type == MsgType::S2M_NDR && ids.size() < 2) {
    ids.push_back(s2m_q_.front().req_id);
    s2m_q_.pop_front();
  }
  std::uint32_t used = static_cast<std::uint32_t>(ids.size()) * kHeaderBytes;
  return PackedFlit{LinkDir::S2M, FlitType::G5, used, kPayloadBytes - used, ids};
}

void CxlMemSimulator::send_flit(PackedFlit flit, SimTime now) {
  SimTime &next_ready = (flit.dir == LinkDir::M2S) ? next_m2s_ready_ : next_s2m_ready_;
  SimTime tx_start = std::max(now, next_ready);
  SimTime queue_delay = tx_start - now;
  next_ready = tx_start + cfg_.serdes_time_per_flit;
  SimTime arrival = next_ready + cfg_.link_latency;

  if (flit.dir == LinkDir::M2S) {
    queue_delay_m2s_ += queue_delay;
    ++m2s_flits_;
  } else {
    queue_delay_s2m_ += queue_delay;
    ++s2m_flits_;
  }

  if (flit.type == FlitType::G0) ++data_flits_;
  else ++header_flits_;

  ++total_flits_;
  utilized_bytes_sum_ += flit.bytes_used_in_payload;
  wasted_bytes_sum_ += flit.bytes_wasted;

  schedule(arrival, [this, flit, arrival]() {
    for (std::uint64_t id : flit.complete_on_arrival) on_completion(id, arrival);
  });
}

void CxlMemSimulator::flush_m2s() {
  while (!m2s_q_.empty()) {
    if (m2s_q_.front().type == MsgType::M2S_REQ) {
      send_flit(build_m2s_h5(), now_);
    } else {
      send_flit(build_m2s_h4_and_pack_reqs(), now_);
      send_flit(build_g0(LinkDir::M2S, 0), now_);
    }
  }
}

void CxlMemSimulator::flush_s2m() {
  while (!s2m_q_.empty()) {
    if (s2m_q_.front().type == MsgType::S2M_DRS) {
      std::vector<PackedFlit> data_flits;
      send_flit(build_s2m_g4_or_g6(data_flits), now_);
      for (auto &f : data_flits) send_flit(f, now_);
    } else {
      send_flit(build_s2m_g5(), now_);
    }
  }
}

void CxlMemSimulator::on_completion(std::uint64_t req_id, SimTime done_at) {
  if (req_id == 0 || req_id >= requests_.size()) return;
  Request &req = requests_[req_id];
  if (req.id == 0) return;

  SimTime latency = done_at - req.issued_at;
  if (req.type == ReqType::READ) {
    ++read_count_;
    read_latency_sum_ += latency;
  } else {
    ++write_count_;
    write_latency_sum_ += latency;
  }

  req.id = 0;
  ++completed_;
  --outstanding_;
  last_completion_time_ = std::max(last_completion_time_, done_at);
  try_issue_requests();
}

SimStats CxlMemSimulator::run() {
  schedule(0, [this]() { try_issue_requests(); });
  process_events();

  SimStats s;
  s.avg_read_latency_ns = read_count_ ? static_cast<double>(read_latency_sum_ / read_count_) : 0.0;
  s.avg_write_latency_ns = write_count_ ? static_cast<double>(write_latency_sum_ / write_count_) : 0.0;
  s.throughput_reqs_per_ns = last_completion_time_ ? static_cast<double>(completed_) / last_completion_time_ : 0.0;
  s.avg_flit_utilization_pct = total_flits_ ? static_cast<double>((utilized_bytes_sum_ / total_flits_) * 100.0 / kPayloadBytes) : 0.0;
  s.avg_bytes_wasted_per_flit = total_flits_ ? static_cast<double>(wasted_bytes_sum_ / total_flits_) : 0.0;
  s.total_m2s_flits = m2s_flits_;
  s.total_s2m_flits = s2m_flits_;
  s.total_data_flits = data_flits_;
  s.total_header_flits = header_flits_;
  return s;
}

}  // namespace cxlmem
