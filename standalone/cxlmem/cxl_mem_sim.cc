#include "cxl_mem_sim.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>

namespace cxlmem {

void EventLoop::schedule(SimTime when, std::function<void()> fn) {
  q_.push(Event{when, seq_++, std::move(fn)});
}

void EventLoop::run() {
  while (!q_.empty()) {
    Event e = q_.top();
    q_.pop();
    now_ = e.time;
    e.fn();
  }
}

void SimStats::recordFlit(const CxlFlit &flit) {
  total_flits_sent++;
  if (flit.direction == Direction::M2S) {
    total_m2s_flits_sent++;
  } else {
    total_s2m_flits_sent++;
  }
  sum_flit_bytes_used += flit.flit_bytes_used;
  sum_flit_slots += flit.slots.size();
  sum_wasted_bytes += flit.wasted_bytes;
}

std::vector<CxlFlit> CxlFlitPacker::packFromQueue(std::deque<CxlSlot> &slot_queue,
                                                  Direction dir,
                                                  uint64_t &next_flit_seq,
                                                  SimTime now,
                                                  bool force_flush) const {
  std::vector<CxlFlit> out;
  if (slot_queue.empty()) {
    return out;
  }

  while (!slot_queue.empty()) {
    CxlFlit flit;
    flit.flit_seq_id = next_flit_seq++;
    flit.direction = dir;
    flit.packed_at = now;

    uint32_t used = 0;
    while (!slot_queue.empty()) {
      const CxlSlot &cand = slot_queue.front();
      if (used + cand.payload_bytes > cfg_.flit_bytes) {
        break;
      }
      flit.slots.push_back(cand);
      used += cand.payload_bytes;
      slot_queue.pop_front();
      if (used == cfg_.flit_bytes) {
        break;
      }
    }

    if (flit.slots.empty()) {
      // Guard against invalid slot sizes larger than flit size.
      CxlSlot giant = slot_queue.front();
      slot_queue.pop_front();
      giant.payload_bytes = cfg_.flit_bytes;
      flit.slots.push_back(giant);
      used = cfg_.flit_bytes;
    }

    flit.flit_bytes_used = used;
    flit.wasted_bytes = cfg_.flit_bytes - used;

    const bool full = used == cfg_.flit_bytes;
    const bool flush_always = (cfg_.flush_policy == FlushPolicy::ALWAYS);
    if (full || force_flush || flush_always || !out.empty() || slot_queue.empty()) {
      out.push_back(std::move(flit));
    } else {
      // ON_DEMAND and not forced with a partial flit: defer for future packing.
      for (auto it = flit.slots.rbegin(); it != flit.slots.rend(); ++it) {
        slot_queue.push_front(*it);
      }
      break;
    }
  }

  return out;
}

uint32_t CxlFlitPacker::estimateFlitsInIsolation(const std::vector<CxlSlot> &slots,
                                                 uint32_t flit_bytes) {
  uint32_t count = 0;
  uint32_t used = 0;
  for (const auto &slot : slots) {
    if (used + slot.payload_bytes > flit_bytes) {
      count++;
      used = 0;
    }
    used += std::min(slot.payload_bytes, flit_bytes);
    if (used == flit_bytes) {
      count++;
      used = 0;
    }
  }
  if (used > 0) {
    count++;
  }
  return count;
}

void CxlLinkModel::sendFlit(CxlFlit flit, Direction dir,
                            std::function<void(const CxlFlit &)> deliver_cb) {
  SimTime &next_ready = (dir == Direction::M2S) ? next_tx_ready_m2s_ : next_tx_ready_s2m_;
  SimTime now = ev_.now();
  SimTime tx_start = std::max(now, next_ready);
  SimTime queue_delay = tx_start - now;
  SimTime tx_done = tx_start + cfg_.serdes_time_per_flit;
  next_ready = tx_done;
  SimTime rx_time = tx_done + cfg_.link_latency;

  flit.tx_queue_enter = now;
  flit.tx_start = tx_start;
  flit.rx_arrive = rx_time;

  if (dir == Direction::M2S) {
    stats_.m2s_queue_delay += queue_delay;
  } else {
    stats_.s2m_queue_delay += queue_delay;
  }
  stats_.recordFlit(flit);

  ev_.schedule(rx_time, [deliver_cb, flit]() { deliver_cb(flit); });
}

void DramBackend::submitRead(uint64_t, uint64_t, uint32_t,
                             std::function<void()> done_cb) {
  ev_.schedule(ev_.now() + cfg_.backend_read_latency, std::move(done_cb));
}

void DramBackend::submitWrite(uint64_t, uint64_t, uint32_t,
                              std::function<void()> done_cb) {
  ev_.schedule(ev_.now() + cfg_.backend_write_latency, std::move(done_cb));
}

bool CxlHostAdapter::canAccept() const {
  return outstanding_txns_.size() < cfg_.max_outstanding;
}

bool CxlHostAdapter::submitCpuRequest(
    const CpuRequest &req,
    std::function<void(const CpuRequest &, SimTime)> done_cb) {
  if (!canAccept() || device_ == nullptr) {
    return false;
  }

  CxlTxn txn;
  txn.txn_id = req.req_id;
  txn.type = req.type;
  txn.address = req.address;
  txn.size = req.size;
  txn.direction = Direction::M2S;
  txn.created_at = ev_.now();

  std::vector<CxlSlot> slots;
  generateM2SSlots(req, txn, slots);
  txn.total_slots_generated = slots.size();

  outstanding_txns_[txn.txn_id] = txn;
  cpu_requests_[req.req_id] = req;
  completion_cb_[req.req_id] = std::move(done_cb);

  ev_.schedule(ev_.now() + cfg_.packetize_delay, [this, slots]() {
    for (const auto &s : slots) {
      m2s_slot_q_.push_back(s);
    }
    flushM2S(true);
  });

  return true;
}

void CxlHostAdapter::generateM2SSlots(const CpuRequest &req, CxlTxn &txn,
                                      std::vector<CxlSlot> &out_slots) {
  auto push_slot = [&](MsgType msg, SlotKind kind, uint32_t bytes, bool extra) {
    CxlSlot s;
    s.txn_id = txn.txn_id;
    s.msg_type = msg;
    s.slot_kind = kind;
    s.payload_bytes = bytes;
    s.is_header_like = (kind == SlotKind::HEADER);
    s.is_data_like = (kind == SlotKind::DATA);
    s.is_extra_experimental = extra;
    s.generated_at = ev_.now();
    out_slots.push_back(s);
  };

  if (req.type == TxnType::READ) {
    push_slot(MsgType::M2S_RD_REQ, SlotKind::HEADER, 32, false);
    push_slot(MsgType::M2S_RD_REQ, SlotKind::DATA, 16, false);
    stats_.total_slots_m2s_rd_req += 2;
  } else {
    uint32_t total = cfg_.base_m2s_write_slots +
                     (cfg_.extra_slot_enable_m2s_write ? 1U : 0U);
    push_slot(MsgType::M2S_WR_REQ, SlotKind::HEADER, 32, false);
    for (uint32_t i = 1; i < total; ++i) {
      bool extra = cfg_.extra_slot_enable_m2s_write && i == total - 1;
      push_slot(MsgType::M2S_WR_REQ, extra ? SlotKind::EXTRA : SlotKind::DATA,
                extra ? 24 : 72, extra);
      if (extra) {
        stats_.total_extra_slots++;
      }
    }
    stats_.total_slots_m2s_wr_req += total;
  }

  for (uint32_t i = 0; i < out_slots.size(); ++i) {
    out_slots[i].slot_index = i;
    out_slots[i].total_slots_for_txn = out_slots.size();
  }

  // "changed flit count" is evaluated in isolation to keep this local and cheap.
  if (req.type == TxnType::WRITE) {
    CxlConfig baseline = cfg_;
    baseline.extra_slot_enable_m2s_write = false;
    std::vector<CxlSlot> baseline_slots;
    // Generate baseline-like sequence without mutating global stats.
    baseline_slots.push_back(CxlSlot{txn.txn_id, MsgType::M2S_WR_REQ, SlotKind::HEADER,
                                     32, true, false, false, 0, 0, ev_.now()});
    for (uint32_t i = 1; i < baseline.base_m2s_write_slots; ++i) {
      baseline_slots.push_back(CxlSlot{txn.txn_id, MsgType::M2S_WR_REQ, SlotKind::DATA,
                                       72, false, true, false, i, 0, ev_.now()});
    }
    for (uint32_t i = 0; i < baseline_slots.size(); ++i) {
      baseline_slots[i].slot_index = i;
      baseline_slots[i].total_slots_for_txn = baseline_slots.size();
    }

    uint32_t baseline_flits =
        CxlFlitPacker::estimateFlitsInIsolation(baseline_slots, cfg_.flit_bytes);
    uint32_t current_flits =
        CxlFlitPacker::estimateFlitsInIsolation(out_slots, cfg_.flit_bytes);
    if (baseline_flits != current_flits) {
      stats_.requests_with_changed_flit_count++;
    }
  }
}

void CxlHostAdapter::flushM2S(bool force) {
  auto flits = packer_.packFromQueue(m2s_slot_q_, Direction::M2S, next_flit_seq_,
                                     ev_.now(), force);
  for (auto &f : flits) {
    link_.sendFlit(std::move(f), Direction::M2S,
                   [this](const CxlFlit &arrived) { device_->onM2SFlit(arrived); });
  }
}

void CxlHostAdapter::onS2MFlit(const CxlFlit &flit) {
  ev_.schedule(ev_.now() + cfg_.depacketize_delay, [this, flit]() {
    for (const auto &slot : flit.slots) {
      auto it = outstanding_txns_.find(slot.txn_id);
      if (it == outstanding_txns_.end()) {
        continue;
      }
      CxlTxn &txn = it->second;
      txn.total_slots_received++;
      if (slot.total_slots_for_txn > 0 &&
          txn.total_slots_received >= slot.total_slots_for_txn) {
        txn.complete = true;
        txn.completed_at = ev_.now();

        CpuRequest req = cpu_requests_[txn.txn_id];
        SimTime latency = txn.completed_at - req.issued_at;
        if (req.type == TxnType::READ) {
          stats_.read_count++;
          stats_.read_latency_sum += latency;
        } else {
          stats_.write_count++;
          stats_.write_latency_sum += latency;
        }

        auto cb = completion_cb_[txn.txn_id];
        cb(req, ev_.now());

        completion_cb_.erase(txn.txn_id);
        cpu_requests_.erase(txn.txn_id);
        outstanding_txns_.erase(txn.txn_id);
      }
    }
  });
}

void CxlDeviceAdapter::onM2SFlit(const CxlFlit &flit) {
  ev_.schedule(ev_.now() + cfg_.depacketize_delay, [this, flit]() {
    for (const auto &slot : flit.slots) {
      RxAssembly &rx = rx_assembly_[slot.txn_id];
      rx.msg_type = slot.msg_type;
      rx.total_slots = slot.total_slots_for_txn;
      rx.received_slots++;
      // Minimal model: reconstruct synthetic addr/size deterministically.
      rx.addr = slot.txn_id * 64;
      rx.size = 64;
      maybeCompleteRxTxn(slot.txn_id);
    }
  });
}

void CxlDeviceAdapter::maybeCompleteRxTxn(uint64_t txn_id) {
  auto it = rx_assembly_.find(txn_id);
  if (it == rx_assembly_.end()) {
    return;
  }
  if (it->second.total_slots == 0 || it->second.received_slots < it->second.total_slots) {
    return;
  }

  RxAssembly rx = it->second;
  rx_assembly_.erase(it);
  processCompleteTxn(txn_id, rx);
}

void CxlDeviceAdapter::processCompleteTxn(uint64_t txn_id, const RxAssembly &rx) {
  if (rx.msg_type == MsgType::M2S_RD_REQ) {
    dram_.submitRead(txn_id, rx.addr, rx.size, [this, txn_id, rx]() {
      std::vector<CxlSlot> rsp_slots;
      generateS2MSlots(txn_id, rx.addr, rx.size, rsp_slots);
      ev_.schedule(ev_.now() + cfg_.packetize_delay, [this, rsp_slots]() {
        for (const auto &s : rsp_slots) {
          s2m_slot_q_.push_back(s);
        }
        flushS2M(true);
      });
    });
  } else {
    dram_.submitWrite(txn_id, rx.addr, rx.size, [this, txn_id, rx]() {
      std::vector<CxlSlot> rsp_slots;
      CxlSlot ndr;
      ndr.txn_id = txn_id;
      ndr.msg_type = MsgType::S2M_NDR;
      ndr.slot_kind = SlotKind::HEADER;
      ndr.payload_bytes = 24;
      ndr.is_header_like = true;
      ndr.slot_index = 0;
      ndr.total_slots_for_txn = 1;
      ndr.generated_at = ev_.now();
      rsp_slots.push_back(ndr);
      stats_.total_slots_s2m_ndr += 1;
      ev_.schedule(ev_.now() + cfg_.packetize_delay, [this, rsp_slots]() {
        for (const auto &s : rsp_slots) {
          s2m_slot_q_.push_back(s);
        }
        flushS2M(true);
      });
    });
  }
}

void CxlDeviceAdapter::generateS2MSlots(uint64_t txn_id, uint64_t, uint32_t,
                                        std::vector<CxlSlot> &out_slots) {
  auto push_slot = [&](SlotKind kind, uint32_t bytes, bool extra) {
    CxlSlot s;
    s.txn_id = txn_id;
    s.msg_type = MsgType::S2M_DRSP;
    s.slot_kind = kind;
    s.payload_bytes = bytes;
    s.is_header_like = (kind == SlotKind::HEADER);
    s.is_data_like = (kind == SlotKind::DATA);
    s.is_extra_experimental = extra;
    s.generated_at = ev_.now();
    out_slots.push_back(s);
  };

  uint32_t total = cfg_.base_s2m_read_rsp_slots +
                   (cfg_.extra_slot_enable_s2m_read_rsp ? 1U : 0U);
  push_slot(SlotKind::HEADER, 32, false);
  for (uint32_t i = 1; i < total; ++i) {
    bool extra = cfg_.extra_slot_enable_s2m_read_rsp && i == total - 1;
    push_slot(extra ? SlotKind::EXTRA : SlotKind::DATA, extra ? 24 : 72, extra);
    if (extra) {
      stats_.total_extra_slots++;
    }
  }
  for (uint32_t i = 0; i < out_slots.size(); ++i) {
    out_slots[i].slot_index = i;
    out_slots[i].total_slots_for_txn = out_slots.size();
  }
  stats_.total_slots_s2m_drsp += total;

  std::vector<CxlSlot> baseline_slots;
  baseline_slots.push_back(CxlSlot{txn_id, MsgType::S2M_DRSP, SlotKind::HEADER, 32,
                                   true, false, false, 0, 0, ev_.now()});
  for (uint32_t i = 1; i < cfg_.base_s2m_read_rsp_slots; ++i) {
    baseline_slots.push_back(CxlSlot{txn_id, MsgType::S2M_DRSP, SlotKind::DATA, 72,
                                     false, true, false, i, 0, ev_.now()});
  }
  for (uint32_t i = 0; i < baseline_slots.size(); ++i) {
    baseline_slots[i].slot_index = i;
    baseline_slots[i].total_slots_for_txn = baseline_slots.size();
  }

  uint32_t baseline_flits =
      CxlFlitPacker::estimateFlitsInIsolation(baseline_slots, cfg_.flit_bytes);
  uint32_t current_flits =
      CxlFlitPacker::estimateFlitsInIsolation(out_slots, cfg_.flit_bytes);
  if (baseline_flits != current_flits) {
    stats_.requests_with_changed_flit_count++;
  }
}

void CxlDeviceAdapter::flushS2M(bool force) {
  auto flits = packer_.packFromQueue(s2m_slot_q_, Direction::S2M, next_flit_seq_,
                                     ev_.now(), force);
  for (auto &f : flits) {
    link_.sendFlit(std::move(f), Direction::S2M,
                   [this](const CxlFlit &arrived) { host_->onS2MFlit(arrived); });
  }
}

void SimpleCpuDriver::runSynthetic(uint64_t num_reqs, uint32_t read_percent,
                                   uint32_t size, uint64_t stride_bytes) {
  total_to_issue_ = num_reqs;
  auto issued = std::make_shared<uint64_t>(0);
  auto addr = std::make_shared<uint64_t>(0x100000);

  auto try_issue = std::make_shared<std::function<void()>>();
  *try_issue = [this, read_percent, size, stride_bytes, issued, addr, try_issue]() {
    while (*issued < total_to_issue_ && host_.canAccept()) {
      CpuRequest req;
      req.req_id = next_req_id_++;
      req.type = ((req.req_id * 100 / total_to_issue_) % 100 < read_percent)
                     ? TxnType::READ
                     : TxnType::WRITE;
      req.address = *addr;
      req.size = size;
      req.issued_at = ev_.now();
      *addr += stride_bytes;

      bool accepted = host_.submitCpuRequest(
          req, [this, try_issue](const CpuRequest &, SimTime) {
            completed_++;
            ev_.schedule(ev_.now(), *try_issue);
          });
      if (!accepted) {
        break;
      }
      (*issued)++;
    }
  };

  ev_.schedule(ev_.now(), *try_issue);
}

void SimpleCpuDriver::printSummary() const {
  std::cout << "completed_requests=" << completed_ << "\n";
  if (stats_.read_count) {
    std::cout << "avg_read_latency="
              << (static_cast<double>(stats_.read_latency_sum) / stats_.read_count)
              << "\n";
  }
  if (stats_.write_count) {
    std::cout << "avg_write_latency="
              << (static_cast<double>(stats_.write_latency_sum) / stats_.write_count)
              << "\n";
  }
  if (stats_.total_flits_sent) {
    std::cout << "avg_flit_bytes_used="
              << (static_cast<double>(stats_.sum_flit_bytes_used) /
                  stats_.total_flits_sent)
              << "\n";
    std::cout << "avg_flit_slots="
              << (static_cast<double>(stats_.sum_flit_slots) /
                  stats_.total_flits_sent)
              << "\n";
    std::cout << "avg_wasted_bytes="
              << (static_cast<double>(stats_.sum_wasted_bytes) /
                  stats_.total_flits_sent)
              << "\n";
  }
}

std::string toString(MsgType t) {
  switch (t) {
    case MsgType::M2S_RD_REQ:
      return "M2S_RD_REQ";
    case MsgType::M2S_WR_REQ:
      return "M2S_WR_REQ";
    case MsgType::S2M_NDR:
      return "S2M_NDR";
    case MsgType::S2M_DRSP:
      return "S2M_DRSP";
  }
  return "UNKNOWN";
}

std::string toString(Direction d) {
  return d == Direction::M2S ? "M2S" : "S2M";
}

}  // namespace cxlmem
