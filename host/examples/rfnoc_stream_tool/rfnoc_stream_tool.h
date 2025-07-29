
#include <uhd/exception.hpp>
#include <uhd/rfnoc_graph.hpp>
#include <uhd/utils/graph_utils.hpp>
#include <uhd/rfnoc/graph_edge.hpp>
#include <uhd/rfnoc/noc_block_base.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/rfnoc/ddc_block_control.hpp>
#include <uhd/rfnoc/duc_block_control.hpp>
#include <uhd/rfnoc/fir_filter_block_control.hpp>
#include <uhd/rfnoc/fft_block_control.hpp>
#include <uhd/rfnoc/window_block_control.hpp>
#include <uhd/rfnoc/replay_block_control.hpp>
#include <uhd/rfnoc/siggen_block_control.hpp>
#include <uhd/rfnoc/null_block_control.hpp>
#include <uhd/rfnoc/addsub_block_control.hpp>
#include <uhd/rfnoc/split_stream_block_control.hpp>
#include <uhd/rfnoc/switchboard_block_control.hpp>
#include <uhd/rfnoc/moving_average_block_control.hpp>
#include <uhd/rfnoc/vector_iir_block_control.hpp>
#include <uhd/rfnoc/keep_one_in_n_block_control.hpp>
#include <uhd/rfnoc/mb_controller.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/time_spec.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <yaml-cpp/yaml.h>
#include <chrono>
#include <complex>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <queue>
#include <regex>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array>
#include <ctime>
#include <iomanip>
#include <numeric>


// -------------------------------------------------------------------------------------------------
// Local helper macro: cache‑line size (for alignment) – fallback 64B.
#ifndef CACHELINE_BYTES
#define CACHELINE_BYTES 64
#endif

#define DEFAULT_TICKRATE 200000000
#define MAX_ANALYSIS_PACKETS 1000000
#define DEFAULT_RING_BUFFER_SIZE (1024 * 1024 * 16)  // 16MB default ring buffer per stream

namespace po = boost::program_options;
using namespace std::chrono_literals;

// Global flag for Ctrl+C handling (thread safe version)
static std::atomic_bool stop_signal_called{false};
static std::vector<std::atomic_bool*> g_per_stream_stop_flags; // non‑owning


template<typename T>
class SPSCRingBuffer {
public:
    SPSCRingBuffer(size_t capacity) 
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(std::make_unique<T[]>(capacity))
        , write_pos_(0)
        , read_pos_(0)
        , cached_read_pos_(0)
        , cached_write_pos_(0)
    {
        // Ensure capacity is power of 2
        if ((capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Ring buffer capacity must be power of 2");
        }
    }

    bool push(T&& item) {
        const auto current_write = write_pos_.load(std::memory_order_relaxed);
        const auto next_write = (current_write + 1) & mask_;
        
        // Check if buffer is full
        if (next_write == cached_read_pos_) {
            cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
            if (next_write == cached_read_pos_) {
                return false; // Buffer full
            }
        }
        
        buffer_[current_write] = std::move(item);
        write_pos_.store(next_write, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const auto current_read = read_pos_.load(std::memory_order_relaxed);
        
        // Check if buffer is empty
        if (current_read == cached_write_pos_) {
            cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
            if (current_read == cached_write_pos_) {
                return false; // Buffer empty
            }
        }
        
        item = std::move(buffer_[current_read]);
        read_pos_.store((current_read + 1) & mask_, std::memory_order_release);
        return true;
    }

    size_t size() const {
        auto write = write_pos_.load(std::memory_order_acquire);
        auto read = read_pos_.load(std::memory_order_acquire);
        if (write >= read) {
            return write - read;
        } else {
            return capacity_ - read + write;
        }
    }

    bool empty() const {
        return write_pos_.load(std::memory_order_acquire) == 
               read_pos_.load(std::memory_order_acquire);
    }

    bool full() const {
        auto write = write_pos_.load(std::memory_order_acquire);
        auto read = read_pos_.load(std::memory_order_acquire);
        return ((write + 1) & mask_) == read;
    }

    size_t capacity() const { return capacity_; }

private:
    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<T[]> buffer_;
    
    // Separate cache lines for producer and consumer
    alignas(CACHELINE_BYTES) std::atomic<size_t> write_pos_;
    alignas(CACHELINE_BYTES) std::atomic<size_t> read_pos_;
    
    // Cached positions to avoid false sharing
    alignas(CACHELINE_BYTES) size_t cached_read_pos_;
    alignas(CACHELINE_BYTES) size_t cached_write_pos_;
};

// ============================================================================
// Packet Buffer Structure for Ring Buffer
// ============================================================================

struct PacketBuffer {
    std::vector<uint8_t> data;
    size_t stream_id;
    uint64_t packet_number;
    uhd::time_spec_t timestamp;
    bool has_timestamp;
    
    PacketBuffer() : stream_id(0), packet_number(0), has_timestamp(false) {}
    
    PacketBuffer(const PacketBuffer&)            = default;  // deep copy OK (vector copies)
    PacketBuffer& operator=(const PacketBuffer&) = default;
    PacketBuffer(PacketBuffer&&) noexcept        = default;
    PacketBuffer& operator=(PacketBuffer&&) noexcept = default;
};

// ============================================================================
// Stream Buffer Configuration
// ============================================================================

struct StreamBufferConfig {
    size_t ring_buffer_size = DEFAULT_RING_BUFFER_SIZE;
    size_t batch_write_size = 100;  // Number of packets to batch before writing
    bool enable_compression = false;
    size_t high_water_mark = 90;    // Percentage full before warning
    size_t low_water_mark = 10;     // Percentage full for low buffer warning
};

// CHDR packet types according to RFNoC spec
enum chdr_packet_type_t {
    PKT_TYPE_MGMT           = 0x0,
    PKT_TYPE_STRS           = 0x1,
    PKT_TYPE_STRC           = 0x2,
    PKT_TYPE_CTRL           = 0x4,
    PKT_TYPE_DATA_NO_TS     = 0x6,
    PKT_TYPE_DATA_WITH_TS   = 0x7
};

// Structure to hold parsed CHDR packet data - enhanced with stream ID
struct chdr_packet_data {
    // Stream identification
    size_t stream_id;
    std::string stream_block;
    size_t stream_port;
    
    // Raw header as captured
    uint64_t header_raw;
    
    // Parsed header fields
    uint16_t dst_epid;
    uint16_t length;
    uint16_t seq_num;
    uint8_t num_mdata;
    uint8_t pkt_type;
    bool eov;
    bool eob;
    uint8_t vc;
    
    // Timestamp (if present)
    uint64_t timestamp;
    bool has_timestamp;
    uint64_t time_seconds;
    
    // Metadata
    std::vector<uint64_t> metadata;
    
    // Payload
    std::vector<uint8_t> payload;
    
    // Parse header from raw 64-bit value (little-endian)
    void parse_header() {
        dst_epid = header_raw & 0xFFFF;
        length = (header_raw >> 16) & 0xFFFF;
        seq_num = (header_raw >> 32) & 0xFFFF;
        num_mdata = (header_raw >> 48) & 0x1F;
        pkt_type = (header_raw >> 53) & 0x7;
        eov = (header_raw >> 56) & 0x1;
        eob = (header_raw >> 57) & 0x1;
        vc = (header_raw >> 58) & 0x3F;
        has_timestamp = (pkt_type == PKT_TYPE_DATA_WITH_TS);
    }
    
    std::string pkt_type_str() const {
        switch(pkt_type) {
            case PKT_TYPE_MGMT: return "Management";
            case PKT_TYPE_STRS: return "Stream Status";
            case PKT_TYPE_STRC: return "Stream Command";
            case PKT_TYPE_CTRL: return "Control";
            case PKT_TYPE_DATA_NO_TS: return "Data (No TS)";
            case PKT_TYPE_DATA_WITH_TS: return "Data (With TS)";
            default: return "Reserved";
        }
    }
};
// PPS reset configuration
struct PpsResetConfig {
    bool enable_pps_reset = false;
    double wait_time_sec = 1.5;  // Time to wait for PPS after reset command
    bool verify_reset = true;    // Verify the reset actually occurred
    double max_time_after_reset = 1.0;  // Max acceptable time after reset for verification
};

// Multi-stream configuration
struct MultiStreamConfig {
    bool enable_multi_stream = false;
    bool sync_streams = true;  // Synchronize stream start times
    bool separate_files = false;  // Write each stream to separate file
    std::string file_prefix = "stream";  // Prefix for separate files
    size_t max_streams = 0;  // 0 = unlimited
    std::vector<std::string> stream_blocks;  // Specific blocks to stream from
    double sync_delay = 0.1;  // Delay before synchronized start (seconds)

    // Ring buffer configuration
    StreamBufferConfig buffer_config;
    
};

// Per-stream capture statistics
struct StreamStats {
    size_t stream_id;
    std::string block_id;
    size_t port;
    size_t packets_captured = 0;
    size_t total_samples = 0;
    size_t overflow_count = 0;
    size_t error_count = 0;
    double first_timestamp = 0.0;
    double last_timestamp = 0.0;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;

    // Ring buffer statistics
    size_t buffer_overflows = 0;
    size_t max_buffer_usage = 0;
    size_t total_bytes_written = 0;
    double avg_buffer_usage = 0.0;
};

// Stream capture context
struct StreamContext {
    size_t stream_id;
    std::string block_id;
    size_t port;
    uhd::rx_streamer::sptr rx_streamer;
    std::ofstream* output_file;
    std::mutex* file_mutex;  // For shared file access
    StreamStats stats;
    std::vector<chdr_packet_data>* analysis_packets;
    std::mutex* analysis_mutex;
    double tick_rate;
    size_t samps_per_buff;
    bool separate_file;
    uhd::time_spec_t pps_reset_time;  // Time when PPS reset occurred
    bool pps_reset_used;

    // Ring buffer for this stream
    std::shared_ptr<SPSCRingBuffer<PacketBuffer>> ring_buffer;
    
    // File writer thread handle
    std::unique_ptr<std::thread> writer_thread;
    
    // Output file path
    std::string output_filename;
    
    // Buffer configuration
    StreamBufferConfig buffer_config;

    /* ------------------------------------------------------------------ *
     *  rule of five – StreamContext is *move‑only* because it owns a     *
     *  std::unique_ptr<std::thread>.                                     *
     * ------------------------------------------------------------------ */
    StreamContext()                                  = default;
    StreamContext(const StreamContext&)              = delete;
    StreamContext& operator=(const StreamContext&)   = delete;
    StreamContext(StreamContext&&)                   = default;
    StreamContext& operator=(StreamContext&&)        = default;
};

// File writer statistics
struct FileWriterStats {
    size_t packets_written = 0;
    size_t bytes_written = 0;
    size_t write_errors = 0;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
};

// Connection info for YAML config
struct ConnectionConfig {
    std::string src_block;
    size_t src_port;
    std::string dst_block;
    size_t dst_port;
    bool is_back_edge = false;  // For handling feedback loops
};

// Switchboard routing configuration
struct SwitchboardConfig {
    std::string block_id;
    std::map<size_t, size_t> connections; // input_port -> output_port
};

// Stream endpoint configuration - enhanced for multi-stream
struct StreamEndpointConfig {
    std::string block_id;
    size_t port;
    std::string direction; // "rx" or "tx"
    std::map<std::string, std::string> stream_args;
    bool enabled = true;  // Allow disabling specific endpoints
    std::string stream_name;  // Optional name for identification
};

// Signal path configuration for explicit path definition
struct SignalPathConfig {
    std::string name;
    std::vector<ConnectionConfig> connections;
};

// Block chain info
struct BlockChain {
    std::vector<uhd::rfnoc::graph_edge_t> edges;
    std::string first_block;
    std::string last_block;
    size_t first_port;
    size_t last_port;
    bool has_sep = false;
};

// Graph configuration from YAML - enhanced for multi-stream
struct GraphConfig {
    std::vector<ConnectionConfig> dynamic_connections;
    std::vector<SwitchboardConfig> switchboard_configs;
    std::map<std::string, std::map<std::string, std::string>> block_properties;
    std::vector<StreamEndpointConfig> stream_endpoints;
    std::vector<SignalPathConfig> signal_paths;  // Explicit signal paths
    bool auto_connect_radio_to_ddc = true;
    bool auto_find_stream_endpoint = true;
    bool commit_after_each_connection = false;
    
    // Multi-stream configuration
    MultiStreamConfig multi_stream;

    // PPS reset configuration
    PpsResetConfig pps_reset;
    
    // Advanced routing options
    bool discover_static_connections = true;
    bool preserve_static_routes = true;
    std::vector<std::string> block_init_order;  // Specific initialization order
};

// Block information
struct BlockInfo {
    std::string block_id;
    std::string block_type;
    size_t num_input_ports;
    size_t num_output_ports;
    bool has_stream_endpoint;
    std::vector<std::string> properties;
    std::map<std::string, std::string> property_types;
};

// Structure to hold discovered graph topology
struct GraphTopology {
    std::vector<BlockInfo> blocks;
    std::vector<uhd::rfnoc::graph_edge_t> static_connections;
    std::vector<uhd::rfnoc::graph_edge_t> active_connections;
    std::map<std::string, std::vector<size_t>> block_stream_ports;  // Block -> list of ports with SEPs
    std::map<std::string, std::map<size_t, std::string>> downstream_connections; // Block:port -> downstream block
    std::map<std::string, std::map<size_t, std::string>> upstream_connections;   // Block:port -> upstream block
};

// File header structure for CHDR capture files with PPS reset support
struct ChdrFileHeader {
    uint32_t magic = 0x43484452;  // "CHDR"
    uint32_t version = 4;          // Version 4 includes ring buffer support, multi-stream and PPS reset info
    uint32_t chdr_width = 64;
    double tick_rate = DEFAULT_TICKRATE;
    uint32_t pps_reset_used = 0;
    double pps_reset_time_sec = 0.0;
    uint32_t num_streams = 0;
    uint32_t ring_buffer_used = 1;  // New field
    uint32_t reserved[5] = {0};    // Reserved for future use
};

// Per-stream header for multi-stream files
struct StreamHeader {
    uint32_t stream_id;
    char block_id[64];
    uint32_t port;
    uint32_t ring_buffer_size;  // New field
    uint32_t reserved[5] = {0};
};


void sig_int_handler(int);
// void print_graph_info(uhd::rfnoc::rfnoc_graph::sptr graph);
std::vector<BlockInfo> discover_blocks_enhanced(uhd::rfnoc::rfnoc_graph::sptr graph);
bool auto_connect_radio_to_ddc(uhd::rfnoc::rfnoc_graph::sptr graph);
template<typename T>
void write_le(std::vector<uint8_t>& buffer, T value);
template<typename T>
T read_le(const uint8_t* data);
void file_writer_thread(
    StreamContext& ctx,
    std::atomic<bool>& stop_writing,
    FileWriterStats& writer_stats);
void capture_stream_ringbuffer(StreamContext& ctx, 
                              std::atomic<bool>& start_capture,
                              std::atomic<bool>& stop_writing,
                              size_t num_packets,
                              FileWriterStats& writer_stats);
uhd::time_spec_t perform_pps_reset(uhd::rfnoc::rfnoc_graph::sptr graph, 
                                  const PpsResetConfig& config);
bool auto_connect_radio_to_ddc(uhd::rfnoc::rfnoc_graph::sptr graph);
GraphConfig load_graph_config(const std::string& yaml_file);
bool connection_exists(uhd::rfnoc::rfnoc_graph::sptr graph,
                      const std::string& src_block, size_t src_port,
                      const std::string& dst_block, size_t dst_port);
bool apply_signal_paths(uhd::rfnoc::rfnoc_graph::sptr graph,
                       const std::vector<SignalPathConfig>& signal_paths);
std::vector<std::pair<std::string, size_t>> find_all_stream_endpoints_enhanced(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const std::vector<StreamEndpointConfig>& configured_endpoints,
    const MultiStreamConfig& multi_config);
GraphTopology discover_graph_topology(uhd::rfnoc::rfnoc_graph::sptr graph);
std::map<std::string, std::string> get_block_property_template(
    const std::string& block_type,
    const BlockInfo& block_info,
    uhd::rfnoc::rfnoc_graph::sptr graph);
void write_dynamic_yaml_template(uhd::rfnoc::rfnoc_graph::sptr graph, 
                                const std::string& filename);
bool apply_block_properties(uhd::rfnoc::rfnoc_graph::sptr graph,
                           const std::map<std::string, std::map<std::string, std::string>>& properties,
                           double default_rate = 1e6);
bool configure_switchboards(uhd::rfnoc::rfnoc_graph::sptr graph,
                           const std::vector<SwitchboardConfig>& configs);
bool apply_dynamic_connections(uhd::rfnoc::rfnoc_graph::sptr graph,
                              const std::vector<ConnectionConfig>& connections,
                              bool commit_after_each = false);
void print_graph_info(const uhd::rfnoc::rfnoc_graph::sptr& graph);
void analyze_packets_with_pps_reset(const std::vector<chdr_packet_data>& packets, 
                                    const std::string& csv_file,
                                    double tick_rate,
                                    const std::vector<StreamStats>& stream_stats,
                                    uhd::time_spec_t pps_reset_time,
                                    bool pps_reset_used,
                                    size_t samps_per_buff,
                                    double rate);
void write_file_header(std::ofstream& file, double tick_rate, 
                      bool pps_reset_used, uhd::time_spec_t pps_reset_time,
                      size_t num_streams);
void write_stream_header(std::ofstream& file, size_t stream_id, 
                        const std::string& block_id, size_t port);
void analyze_packets(const std::vector<chdr_packet_data>& packets, 
                    const std::string& csv_file,
                    double tick_rate,
                    const std::vector<StreamStats>& stream_stats);
template <typename samp_type>
void capture_stream(StreamContext& ctx, 
                   std::atomic<bool>& start_capture,
                   size_t num_packets);