#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cxlmem {

using SimTime = uint64_t;

struct Event {
  SimTime time;
  uint64_t seq;
  std::function<void()> fn;
  bool operator<(const Event &other) const {
    if (time != other.time) {
      return time > other.time;
    }
    return seq > other.seq;
  }
};

class EventLoop {
 public:
  void schedule(SimTime when, std::function<void()> fn);
  void run();
  SimTime now() const { return now_; }

 private:
  SimTime now_ = 0;
  uint64_t seq_ = 0;
  std::priority_queue<Event> q_;
};

enum class Direction { M2S, S2M };
enum class MsgType { M2S_RD_REQ, M2S_WR_REQ, S2M_NDR, S2M_DRSP };
enum class SlotKind { HEADER, DATA, EXTRA };
enum class TxnType { READ, WRITE };
enum class PackingPolicy { FIFO_MIX };
enum class FlushPolicy { ON_DEMAND, ALWAYS };

struct CxlConfig {
  uint32_t flit_bytes = 256;
  uint32_t base_m2s_write_slots = 4;
  uint32_t base_s2m_read_rsp_slots = 4;
  bool extra_slot_enable_m2s_write = false;
  bool extra_slot_enable_s2m_read_rsp = false;

  SimTime packetize_delay = 2;
  SimTime depacketize_delay = 2;
  SimTime serdes_time_per_flit = 4;
  SimTime link_latency = 10;
  SimTime backend_read_latency = 40;
  SimTime backend_write_latency = 30;

  uint32_t max_outstanding = 32;
  PackingPolicy packing_policy = PackingPolicy::FIFO_MIX;
  FlushPolicy flush_policy = FlushPolicy::ON_DEMAND;

  bool debug = false;
};

struct CxlSlot {
  uint64_t txn_id = 0;
  MsgType msg_type = MsgType::M2S_RD_REQ;
  SlotKind slot_kind = SlotKind::HEADER;
  uint32_t payload_bytes = 0;
  bool is_header_like = false;
  bool is_data_like = false;
  bool is_extra_experimental = false;
  uint32_t slot_index = 0;
  uint32_t total_slots_for_txn = 0;
  SimTime generated_at = 0;
};

struct CxlFlit {
  uint64_t flit_seq_id = 0;
  Direction direction = Direction::M2S;
  std::vector<CxlSlot> slots;
  uint32_t flit_bytes_used = 0;
  uint32_t wasted_bytes = 0;

  SimTime packed_at = 0;
  SimTime tx_queue_enter = 0;
  SimTime tx_start = 0;
  SimTime rx_arrive = 0;
  std::string debug_tag;
};

struct CxlTxn {
  uint64_t txn_id = 0;
  TxnType type = TxnType::READ;
  uint64_t address = 0;
  uint32_t size = 64;
  Direction direction = Direction::M2S;
  uint32_t total_slots_generated = 0;
  uint32_t total_slots_received = 0;
  bool complete = false;
  SimTime created_at = 0;
  SimTime completed_at = 0;
};

struct CpuRequest {
  uint64_t req_id = 0;
  TxnType type = TxnType::READ;
  uint64_t address = 0;
  uint32_t size = 64;
  SimTime issued_at = 0;
};

struct SimStats {
  uint64_t total_m2s_flits_sent = 0;
  uint64_t total_s2m_flits_sent = 0;
  uint64_t total_flits_sent = 0;

  uint64_t total_slots_m2s_rd_req = 0;
  uint64_t total_slots_m2s_wr_req = 0;
  uint64_t total_slots_s2m_drsp = 0;
  uint64_t total_slots_s2m_ndr = 0;
  uint64_t total_extra_slots = 0;

  uint64_t sum_flit_bytes_used = 0;
  uint64_t sum_flit_slots = 0;
  uint64_t sum_wasted_bytes = 0;

  uint64_t m2s_queue_delay = 0;
  uint64_t s2m_queue_delay = 0;

  uint64_t read_count = 0;
  uint64_t write_count = 0;
  uint64_t read_latency_sum = 0;
  uint64_t write_latency_sum = 0;

  uint64_t requests_with_changed_flit_count = 0;

  void recordFlit(const CxlFlit &flit);
};

class CxlFlitPacker {
 public:
  explicit CxlFlitPacker(const CxlConfig &cfg) : cfg_(cfg) {}

  std::vector<CxlFlit> packFromQueue(std::deque<CxlSlot> &slot_queue, Direction dir,
                                     uint64_t &next_flit_seq, SimTime now,
                                     bool force_flush) const;

  static uint32_t estimateFlitsInIsolation(const std::vector<CxlSlot> &slots,
                                           uint32_t flit_bytes);

 private:
  const CxlConfig &cfg_;
};

class CxlLinkModel {
 public:
  CxlLinkModel(EventLoop &ev, const CxlConfig &cfg, SimStats &stats)
      : ev_(ev), cfg_(cfg), stats_(stats) {}

  void sendFlit(CxlFlit flit, Direction dir,
                std::function<void(const CxlFlit &)> deliver_cb);

 private:
  EventLoop &ev_;
  const CxlConfig &cfg_;
  SimStats &stats_;

  SimTime next_tx_ready_m2s_ = 0;
  SimTime next_tx_ready_s2m_ = 0;
};

class DramBackend {
 public:
  DramBackend(EventLoop &ev, const CxlConfig &cfg) : ev_(ev), cfg_(cfg) {}

  void submitRead(uint64_t txn_id, uint64_t addr, uint32_t size,
                  std::function<void()> done_cb);
  void submitWrite(uint64_t txn_id, uint64_t addr, uint32_t size,
                   std::function<void()> done_cb);

 private:
  EventLoop &ev_;
  const CxlConfig &cfg_;
};

class CxlDeviceAdapter;

class CxlHostAdapter {
 public:
  CxlHostAdapter(EventLoop &ev, const CxlConfig &cfg, CxlLinkModel &link,
                 SimStats &stats)
      : ev_(ev), cfg_(cfg), link_(link), stats_(stats), packer_(cfg) {}

  void setDevice(CxlDeviceAdapter *device) { device_ = device; }
  bool canAccept() const;
  bool submitCpuRequest(const CpuRequest &req,
                        std::function<void(const CpuRequest &, SimTime)> done_cb);

  void onS2MFlit(const CxlFlit &flit);

 private:
  void generateM2SSlots(const CpuRequest &req, CxlTxn &txn,
                        std::vector<CxlSlot> &out_slots);
  void flushM2S(bool force);

  EventLoop &ev_;
  const CxlConfig &cfg_;
  CxlLinkModel &link_;
  SimStats &stats_;
  CxlFlitPacker packer_;
  CxlDeviceAdapter *device_ = nullptr;

  uint64_t next_flit_seq_ = 1;
  std::deque<CxlSlot> m2s_slot_q_;

  std::unordered_map<uint64_t, CxlTxn> outstanding_txns_;
  std::unordered_map<uint64_t, CpuRequest> cpu_requests_;
  std::unordered_map<uint64_t, std::function<void(const CpuRequest &, SimTime)>>
      completion_cb_;
};

class CxlDeviceAdapter {
 public:
  CxlDeviceAdapter(EventLoop &ev, const CxlConfig &cfg, CxlLinkModel &link,
                   DramBackend &dram, SimStats &stats)
      : ev_(ev), cfg_(cfg), link_(link), dram_(dram), stats_(stats), packer_(cfg) {}

  void setHost(CxlHostAdapter *host) { host_ = host; }
  void onM2SFlit(const CxlFlit &flit);

 private:
  struct RxAssembly {
    MsgType msg_type = MsgType::M2S_RD_REQ;
    uint32_t total_slots = 0;
    uint32_t received_slots = 0;
    uint64_t addr = 0;
    uint32_t size = 64;
  };

  void maybeCompleteRxTxn(uint64_t txn_id);
  void processCompleteTxn(uint64_t txn_id, const RxAssembly &rx);
  void generateS2MSlots(uint64_t txn_id, uint64_t addr, uint32_t size,
                        std::vector<CxlSlot> &out_slots);
  void flushS2M(bool force);

  EventLoop &ev_;
  const CxlConfig &cfg_;
  CxlLinkModel &link_;
  DramBackend &dram_;
  SimStats &stats_;
  CxlFlitPacker packer_;
  CxlHostAdapter *host_ = nullptr;

  uint64_t next_flit_seq_ = 1000000;
  std::deque<CxlSlot> s2m_slot_q_;
  std::unordered_map<uint64_t, RxAssembly> rx_assembly_;
};

class SimpleCpuDriver {
 public:
  SimpleCpuDriver(EventLoop &ev, const CxlConfig &cfg, CxlHostAdapter &host,
                  SimStats &stats)
      : ev_(ev), cfg_(cfg), host_(host), stats_(stats) {}

  void runSynthetic(uint64_t num_reqs, uint32_t read_percent, uint32_t size,
                    uint64_t stride_bytes);
  void printSummary() const;

 private:
  EventLoop &ev_;
  const CxlConfig &cfg_;
  CxlHostAdapter &host_;
  SimStats &stats_;

  uint64_t next_req_id_ = 1;
  uint64_t completed_ = 0;
  uint64_t total_to_issue_ = 0;
};

std::string toString(MsgType t);
std::string toString(Direction d);

}  // namespace cxlmem
