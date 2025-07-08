//
// Copyright 2024 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CHDR Packet Capture Tool - Enhanced with Multi-Stream Support
//
// This utility captures sample data via UHD streaming API with support for:
// - Multiple simultaneous streams from different DDC/endpoints
// - All RFNoC block types (Radio, DDC, DUC, FFT, FIR, etc.)
// - Dynamic and static block chain discovery
// - Comprehensive YAML-based configuration
// - Multi-threaded capture with per-stream threads
// - Stream synchronization and timing alignment
//

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
#include <ctime>
#include <iomanip>
#include <numeric>

#define DEFAULT_TICKRATE 200000000

namespace po = boost::program_options;
using namespace std::chrono_literals;

// Global flag for Ctrl+C handling
static volatile bool stop_signal_called = false;
void print_graph_info(uhd::rfnoc::rfnoc_graph::sptr graph);
void sig_int_handler(int)
{
    stop_signal_called = true;
}

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

// Multi-stream configuration
struct MultiStreamConfig {
    bool enable_multi_stream = false;
    bool sync_streams = true;  // Synchronize stream start times
    bool separate_files = false;  // Write each stream to separate file
    std::string file_prefix = "stream";  // Prefix for separate files
    size_t max_streams = 0;  // 0 = unlimited
    std::vector<std::string> stream_blocks;  // Specific blocks to stream from
    double sync_delay = 0.1;  // Delay before synchronized start (seconds)
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
    bool auto_connect_radio_to_ddc = true;
    bool auto_find_stream_endpoint = true;
    bool commit_after_each_connection = false;
    
    // Multi-stream configuration
    MultiStreamConfig multi_stream;
    
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


// Enhanced block discovery with property detection


std::vector<BlockInfo> discover_blocks_enhanced(uhd::rfnoc::rfnoc_graph::sptr graph) {
    std::vector<BlockInfo> blocks;
    
    // Find all blocks
    auto block_ids = graph->find_blocks("");
    
    for (const auto& id : block_ids) {
        BlockInfo info;
        info.block_id = id.to_string();
        
        auto block = graph->get_block(id);
        if (block) {
            info.block_type = id.get_block_name();
            info.num_input_ports = block->get_num_input_ports();
            info.num_output_ports = block->get_num_output_ports();
            
            // Enhanced SEP detection based on block type and capabilities
            info.has_stream_endpoint = false;
            
            // Use dynamic casting to check actual capabilities
            try {
                // Radio blocks always have SEP
                if (std::dynamic_pointer_cast<uhd::rfnoc::radio_control>(block)) {
                    info.has_stream_endpoint = true;
                }
                // DDC blocks have SEP
                else if (std::dynamic_pointer_cast<uhd::rfnoc::ddc_block_control>(block)) {
                    info.has_stream_endpoint = true;
                }
                // DUC blocks have SEP
                else if (std::dynamic_pointer_cast<uhd::rfnoc::duc_block_control>(block)) {
                    info.has_stream_endpoint = true;
                }
                // Replay blocks have SEP
                else if (std::dynamic_pointer_cast<uhd::rfnoc::replay_block_control>(block)) {
                    info.has_stream_endpoint = true;
                }
                // DMA FIFO blocks have SEP
                else if (info.block_type == "DmaFIFO") {
                    info.has_stream_endpoint = true;
                }
                // Signal generator blocks have SEP
                else if (std::dynamic_pointer_cast<uhd::rfnoc::siggen_block_control>(block)) {
                    info.has_stream_endpoint = true;
                }
                // Null source/sink can have SEP
                else if (info.block_type == "NullSrcSink") {
                    info.has_stream_endpoint = true;
                }
            } catch (...) {
                // If casting fails, fall back to name-based detection
                if (info.block_type == "Radio" || 
                    info.block_type == "DDC" ||
                    info.block_type == "DUC" ||
                    info.block_type == "Replay" ||
                    info.block_type == "DmaFIFO" ||
                    info.block_type == "SigGen" ||
                    id.to_string().find("SEP") != std::string::npos) {
                    info.has_stream_endpoint = true;
                }
            }
            
            // Get available properties with better error handling
            try {
                info.properties = block->get_property_ids();
                
                // Try to determine property types for common properties
                for (const auto& prop : info.properties) {
                    // This is a simplified approach - actual implementation would need
                    // to query property types properly
                    if (prop.find("freq") != std::string::npos ||
                        prop.find("rate") != std::string::npos ||
                        prop.find("gain") != std::string::npos ||
                        prop.find("bandwidth") != std::string::npos) {
                        info.property_types[prop] = "double";
                    } else if (prop.find("enable") != std::string::npos) {
                        info.property_types[prop] = "bool";
                    } else if (prop.find("antenna") != std::string::npos ||
                              prop.find("waveform") != std::string::npos) {
                        info.property_types[prop] = "string";
                    }
                }
            } catch (...) {}
        }
        
        blocks.push_back(info);
    }
    
    return blocks;
}

// Enhanced function to find all stream endpoints with better DDC port detection
std::vector<std::pair<std::string, size_t>> find_all_stream_endpoints_enhanced(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const std::vector<StreamEndpointConfig>& configured_endpoints,
    const MultiStreamConfig& multi_config) {
    
    std::vector<std::pair<std::string, size_t>> endpoints;
    
    // First, add explicitly configured and enabled endpoints
    for (const auto& ep : configured_endpoints) {
        if (ep.enabled && ep.direction == "rx") {
            // Verify the endpoint actually exists
            try {
                uhd::rfnoc::block_id_t block_id(ep.block_id);
                auto block = graph->get_block(block_id);
                if (block && ep.port < block->get_num_output_ports()) {
                    endpoints.push_back({ep.block_id, ep.port});
                } else {
                    std::cerr << "Warning: Configured endpoint " << ep.block_id 
                             << ":" << ep.port << " not found or invalid port" << std::endl;
                }
            } catch (...) {
                std::cerr << "Warning: Could not verify endpoint " << ep.block_id << std::endl;
            }
        }
    }
    
    // If we have specific stream blocks configured, add those
    if (!multi_config.stream_blocks.empty()) {
        for (const auto& block_id : multi_config.stream_blocks) {
            try {
                uhd::rfnoc::block_id_t id(block_id);
                auto block = graph->get_block(id);
                
                if (!block) {
                    std::cerr << "Warning: Configured stream block " << block_id 
                             << " not found" << std::endl;
                    continue;
                }
                
                // For DDC blocks, detect which ports are actually free for streaming
                if (id.get_block_name() == "DDC") {
                    auto ddc = std::dynamic_pointer_cast<uhd::rfnoc::ddc_block_control>(block);
                    if (ddc) {
                        // Check all output ports
                        for (size_t port = 0; port < ddc->get_num_output_ports(); ++port) {
                            // Check if already in endpoints
                            bool already_added = false;
                            for (const auto& [bid, p] : endpoints) {
                                if (bid == block_id && p == port) {
                                    already_added = true;
                                    break;
                                }
                            }
                            
                            if (!already_added) {
                                // Check if port is connected downstream
                                bool has_downstream = false;
                                auto edges = graph->enumerate_active_connections();
                                for (const auto& edge : edges) {
                                    if (edge.src_blockid == block_id && edge.src_port == port) {
                                        // Is the next block a real processing block or just the SEP?
                                        // A SEP (or NullSrcSink) means the port *is* a stream endpoint
                                        // and must **not** be skipped.
                                        if (edge.dst_blockid.find("SEP") == std::string::npos &&
                                            edge.dst_blockid.find("NullSrcSink") == std::string::npos)
                                        {
                                            has_downstream = true;   // real processing block – skip
                                        }
                                        break;
                                    }
                                }
                                
                                if (!has_downstream) {
                                    endpoints.push_back({block_id, port});
                                }
                            }
                        }
                    }
                } else {
                    // For non-DDC blocks, check if already added
                    bool already_added = false;
                    for (const auto& [bid, port] : endpoints) {
                        if (bid == block_id) {
                            already_added = true;
                            break;
                        }
                    }
                    if (!already_added) {
                        endpoints.push_back({block_id, 0});
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Warning: Error processing stream block " << block_id 
                         << ": " << e.what() << std::endl;
            }
        }
    }
    
    // Auto-discover endpoints if enabled and we don't have enough configured
    if (multi_config.enable_multi_stream && 
        (endpoints.empty() || (multi_config.max_streams > 0 && endpoints.size() < multi_config.max_streams))) {
        
        auto blocks = discover_blocks_enhanced(graph);
        
        // Priority order for multi-stream endpoints
        std::vector<std::string> priority_order = {
            "DDC", "Radio", "Replay", "DmaFIFO", "SigGen", "DUC"
        };
        
        for (const auto& block_type : priority_order) {
            for (const auto& block : blocks) {
                if (block.block_type == block_type && block.has_stream_endpoint) {
                    
                    // Special handling for DDC blocks
                    if (block_type == "DDC") {
                        auto edges = graph->enumerate_active_connections();
                        auto static_edges = graph->enumerate_static_connections();
                        edges.insert(edges.end(), static_edges.begin(), static_edges.end());
                        
                        for (size_t port = 0; port < block.num_output_ports; ++port) {
                            // Check if already added
                            bool already_added = false;
                            for (const auto& [bid, p] : endpoints) {
                                if (bid == block.block_id && p == port) {
                                    already_added = true;
                                    break;
                                }
                            }
                            
                            if (!already_added) {
                                // Check if this DDC output is terminal (not connected)
                                bool is_terminal = true;
                                for (const auto& edge : edges) {
                                    if (edge.src_blockid == block.block_id && edge.src_port == port) {
                                        // Check if connected to a streaming endpoint
                                        uhd::rfnoc::block_id_t dst_id(edge.dst_blockid);
                                        if (graph->has_block(dst_id)) {
                                            auto dst_block_info = graph->get_block(dst_id);
                                            if (dst_block_info) {
                                                // If connected to processing block, not terminal
                                                is_terminal = false;
                                            }
                                        }
                                        break;
                                    }
                                }
                                
                                if (is_terminal) {
                                    endpoints.push_back(std::make_pair(block.block_id, port));
                                    
                                    // Check if we've reached max_streams
                                    if (multi_config.max_streams > 0 && 
                                        endpoints.size() >= multi_config.max_streams) {
                                        goto done_discovering;
                                    }
                                }
                            }
                        }
                    } else {
                        // For other block types, typically use port 0
                        bool already_added = false;
                        for (const auto& [bid, port] : endpoints) {
                            if (bid == block.block_id) {
                                already_added = true;
                                break;
                            }
                        }
                        
                        if (!already_added) {
                            endpoints.push_back({block.block_id, 0});
                            
                            // Check if we've reached max_streams
                            if (multi_config.max_streams > 0 && 
                                endpoints.size() >= multi_config.max_streams) {
                                goto done_discovering;
                            }
                        }
                    }
                }
            }
        }
    }
    
done_discovering:
    // Apply max_streams limit if specified
    if (multi_config.max_streams > 0 && endpoints.size() > multi_config.max_streams) {
        endpoints.resize(multi_config.max_streams);
    }
    
    // Validate all endpoints
    std::vector<std::pair<std::string, size_t>> valid_endpoints;
    for (const auto& [block_id, port] : endpoints) {
        try {
            uhd::rfnoc::block_id_t id(block_id);
            auto block = graph->get_block(id);
            if (block && port < block->get_num_output_ports()) {
                valid_endpoints.push_back({block_id, port});
            } else {
                std::cerr << "Warning: Invalid endpoint " << block_id 
                         << ":" << port << " - skipping" << std::endl;
            }
        } catch (...) {
            std::cerr << "Warning: Could not validate endpoint " << block_id 
                     << ":" << port << " - skipping" << std::endl;
        }
    }
    
    return valid_endpoints;
}


// Function to generate example connections based on discovered topology
std::vector<ConnectionConfig> suggest_connections(const GraphTopology& topology) {
    std::vector<ConnectionConfig> suggestions;
    
    // Find unconnected Radio outputs
    std::vector<std::pair<std::string, size_t>> unconnected_radio_outputs;
    for (const auto& block : topology.blocks) {
        if (block.block_type == "Radio") {
            for (size_t port = 0; port < block.num_output_ports; ++port) {
                if (topology.downstream_connections.find(block.block_id) == 
                    topology.downstream_connections.end() ||
                    topology.downstream_connections.at(block.block_id).find(port) ==
                    topology.downstream_connections.at(block.block_id).end()) {
                    unconnected_radio_outputs.push_back(std::make_pair(block.block_id, port));
                }
            }
        }
    }
    
    // Find unconnected DDC inputs
    std::vector<std::pair<std::string, size_t>> unconnected_ddc_inputs;
    for (const auto& block : topology.blocks) {
        if (block.block_type == "DDC") {
            for (size_t port = 0; port < block.num_input_ports; ++port) {
                if (topology.upstream_connections.find(block.block_id) == 
                    topology.upstream_connections.end() ||
                    topology.upstream_connections.at(block.block_id).find(port) ==
                    topology.upstream_connections.at(block.block_id).end()) {
                    unconnected_ddc_inputs.push_back(std::make_pair(block.block_id, port));
                }
            }
        }
    }
    
    // Suggest Radio -> DDC connections
    size_t num_connections = std::min(unconnected_radio_outputs.size(), 
                                     unconnected_ddc_inputs.size());
    for (size_t i = 0; i < num_connections; ++i) {
        ConnectionConfig conn;
        conn.src_block = unconnected_radio_outputs[i].first;
        conn.src_port = unconnected_radio_outputs[i].second;
        conn.dst_block = unconnected_ddc_inputs[i].first;
        conn.dst_port = unconnected_ddc_inputs[i].second;
        conn.is_back_edge = false;
        suggestions.push_back(conn);
    }
    
    return suggestions;
}




std::vector<BlockInfo> discover_blocks(uhd::rfnoc::rfnoc_graph::sptr graph) {
    std::vector<BlockInfo> blocks;
    
    // Find all blocks
    auto block_ids = graph->find_blocks("");
    
    for (const auto& id : block_ids) {
        BlockInfo info;
        info.block_id = id.to_string();
        
        auto block = graph->get_block(id);
        if (block) {
            info.block_type = id.get_block_name();
            info.num_input_ports = block->get_num_input_ports();
            info.num_output_ports = block->get_num_output_ports();
            
            // Enhanced SEP detection
            info.has_stream_endpoint = false;
            
            // Check for specific block types that have SEPs
            if (info.block_type == "Radio" || 
                info.block_type == "DmaFIFO" ||
                info.block_type == "Replay" ||
                info.block_type == "DDC" ||     // DDCs can stream
                info.block_type == "DUC" ||     // DUCs can stream
                id.to_string().find("SEP") != std::string::npos) {
                info.has_stream_endpoint = true;
            }
            
            // Try to detect if block has SEP by checking for streaming capability
            try {
                auto props = block->get_property_ids();
                for (const auto& prop : props) {
                    if (prop.find("stream") != std::string::npos) {
                        info.has_stream_endpoint = true;
                        break;
                    }
                }
            } catch (...) {
                // Ignore errors in property detection
            }
            
            // Get available properties
            try {
                info.properties = block->get_property_ids();
            } catch (...) {}
        }
        
        blocks.push_back(info);
    }
    
    return blocks;
}


// Discover complete graph topology including SEPs and connections
GraphTopology discover_graph_topology(uhd::rfnoc::rfnoc_graph::sptr graph) {
    GraphTopology topology;

    std::cout << "Inside discover_graph_topology method" <<std::endl;
    
    // Get all blocks
    topology.blocks = discover_blocks_enhanced(graph);
    
    // Get connections
    topology.static_connections = graph->enumerate_static_connections();
    topology.active_connections = graph->enumerate_active_connections();
    
    // Build connection maps
    auto all_connections = topology.static_connections;
    all_connections.insert(all_connections.end(), 
                          topology.active_connections.begin(), 
                          topology.active_connections.end());
    
    // Discover stream endpoints - find blocks that connect TO SEPs
    for (const auto& edge : all_connections) {
        if (edge.dst_blockid.find("SEP") != std::string::npos) {
            // This edge connects to a SEP, so the source block/port can stream
            topology.block_stream_ports[edge.src_blockid].push_back(edge.src_port);
        }
    }
    
    // Also check for blocks with streaming capability that might not have SEPs
    for (const auto& block : topology.blocks) {
        if (block.has_stream_endpoint) {
            // Skip if already found via SEP connection
            if (topology.block_stream_ports.find(block.block_id) != topology.block_stream_ports.end()) {
                continue;
            }
            
            // For blocks like Radio that might not show SEP connections in the static list
            bool has_sep_connection = false;
            for (const auto& edge : all_connections) {
                if (edge.src_blockid == block.block_id && 
                    edge.dst_blockid.find("SEP") != std::string::npos) {
                    has_sep_connection = true;
                    break;
                }
            }
            
            if (!has_sep_connection && (block.block_type == "Radio" || 
                                       block.block_type == "DmaFIFO" ||
                                       block.block_type == "Replay")) {
                // These blocks can stream even without visible SEP connections
                for (size_t port = 0; port < block.num_output_ports; ++port) {
                    topology.block_stream_ports[block.block_id].push_back(port);
                }
            }
        }
    }
    
    std::cout << "About to return the created topoplogy" <<std::endl;
    return topology;
}

// Get property template for a specific block type
std::map<std::string, std::string> get_block_property_template(
    const std::string& block_type,
    const BlockInfo& block_info,
    uhd::rfnoc::rfnoc_graph::sptr graph) {
    
    std::map<std::string, std::string> properties;
    
    try {
        uhd::rfnoc::block_id_t block_id(block_info.block_id);
        auto block = graph->get_block(block_id);
        
        if (block_type == "Radio") {
            auto radio = std::dynamic_pointer_cast<uhd::rfnoc::radio_control>(block);
            if (radio) {
                size_t num_chans = radio->get_num_output_ports();
                
                // Global properties
                properties["rate"] = "200e6";  // Example default
                
                // Per-channel properties
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    properties["freq/" + std::to_string(chan)] = "1e9";
                    properties["gain/" + std::to_string(chan)] = "30";
                    properties["antenna/" + std::to_string(chan)] = "RX2";
                    properties["bandwidth/" + std::to_string(chan)] = "20e6";
                }
            }
        }
        else if (block_type == "DDC") {
            auto ddc = std::dynamic_pointer_cast<uhd::rfnoc::ddc_block_control>(block);
            if (ddc) {
                size_t num_chans = ddc->get_num_output_ports();
                
                // Per-channel properties
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    properties["freq/" + std::to_string(chan)] = "0";
                    properties["output_rate/" + std::to_string(chan)] = "1e6";
                    
                    // Try to get current values as defaults
                    try {
                        double current_rate = ddc->get_output_rate(chan);
                        if (current_rate > 0) {
                            properties["output_rate/" + std::to_string(chan)] = 
                                std::to_string(static_cast<long long>(current_rate));
                        }
                    } catch (...) {}
                }
            }
        }
        else if (block_type == "DUC") {
            auto duc = std::dynamic_pointer_cast<uhd::rfnoc::duc_block_control>(block);
            if (duc) {
                size_t num_chans = duc->get_num_input_ports();
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    properties["freq/" + std::to_string(chan)] = "0";
                    properties["input_rate/" + std::to_string(chan)] = "1e6";
                }
            }
        }
        else if (block_type == "FIR") {
            auto fir = std::dynamic_pointer_cast<uhd::rfnoc::fir_filter_block_control>(block);
            if (fir) {
                size_t num_chans = fir->get_num_input_ports();
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    // Get current number of taps if possible
                    try {
                        size_t max_taps = fir->get_max_num_coefficients(chan);
                        properties["coeffs/" + std::to_string(chan)] = 
                            "# Max " + std::to_string(max_taps) + " taps: 1,2,3,4,5,4,3,2,1";
                    } catch (...) {
                        properties["coeffs/" + std::to_string(chan)] = "1,2,3,4,5,4,3,2,1";
                    }
                }
            }
        }
        else if (block_type == "FFT") {
            auto fft = std::dynamic_pointer_cast<uhd::rfnoc::fft_block_control>(block);
            if (fft) {
                // Get current FFT size
                try {
                    size_t fft_size = fft->get_length();
                    properties["length"] = std::to_string(fft_size);
                } catch (...) {
                    properties["length"] = "1024";
                }
                
                properties["magnitude"] = "false";  // complex output by default
                properties["direction"] = "forward";
                properties["scaling"] = "1706";  // Default scaling
                properties["shift"] = "normal";
            }
        }
        else if (block_type == "Window") {
            auto window = std::dynamic_pointer_cast<uhd::rfnoc::window_block_control>(block);
            if (window) {
                size_t num_chans = window->get_num_input_ports();
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    try {
                        size_t max_taps = window->get_max_num_coefficients(chan);
                        properties["coeffs/" + std::to_string(chan)] = 
                            "# Max " + std::to_string(max_taps) + " coefficients";
                    } catch (...) {
                        properties["coeffs/" + std::to_string(chan)] = "1,2,3,4,5";
                    }
                }
            }
        }
        else if (block_type == "SigGen") {
            auto siggen = std::dynamic_pointer_cast<uhd::rfnoc::siggen_block_control>(block);
            if (siggen) {
                size_t num_chans = siggen->get_num_output_ports();
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    properties["enable/" + std::to_string(chan)] = "true";
                    properties["waveform/" + std::to_string(chan)] = "sine";
                    properties["amplitude/" + std::to_string(chan)] = "0.5";
                    properties["frequency/" + std::to_string(chan)] = "1000";
                }
            }
        }
        else if (block_type == "Replay") {
            properties["# Note"] = "Replay blocks are configured via API calls";
        }
        else if (block_type == "MovingAverage") {
            auto mavg = std::dynamic_pointer_cast<uhd::rfnoc::moving_average_block_control>(block);
            if (mavg) {
                properties["sum_len"] = "10";
                properties["divisor"] = "10";
            }
        }
        else if (block_type == "VectorIIR") {
            auto viir = std::dynamic_pointer_cast<uhd::rfnoc::vector_iir_block_control>(block);
            if (viir) {
                size_t num_chans = viir->get_num_input_ports();
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    properties["alpha/" + std::to_string(chan)] = "0.9";
                    properties["beta/" + std::to_string(chan)] = "0.1";
                }
            }
        }
        else if (block_type == "KeepOneInN") {
            auto k1n = std::dynamic_pointer_cast<uhd::rfnoc::keep_one_in_n_block_control>(block);
            if (k1n) {
                size_t num_chans = k1n->get_num_input_ports();
                for (size_t chan = 0; chan < num_chans; ++chan) {
                    properties["n/" + std::to_string(chan)] = "10";
                }
            }
        }
        else if (block_type == "DmaFIFO") {
            properties["# Note"] = "DmaFIFO blocks typically don't require runtime configuration";
        }
        else if (block_type == "SplitStream") {
            auto split = std::dynamic_pointer_cast<uhd::rfnoc::split_stream_block_control>(block);
            if (split) {
                properties["# Note"] = "SplitStream routing is typically static";
            }
        }
        else if (block_type == "AddSub") {
            properties["# Note"] = "AddSub blocks have no runtime configurable properties";
        }
        else if (block_type == "Null") {
            properties["# Note"] = "Null blocks have no configurable properties";
        }
        
    } catch (const std::exception& e) {
        properties["# Error"] = std::string("Failed to query block: ") + e.what();
    }
    
    return properties;
}

// Write dynamic YAML template based on discovered hardware
void write_dynamic_yaml_template(uhd::rfnoc::rfnoc_graph::sptr graph, 
                                const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to create YAML template file");
    }
    
    // Discover graph topology
    std::cout << "Discovering RFNoC graph topology..." << std::endl;
    GraphTopology topology = discover_graph_topology(graph);


    file << "# RFNoC Graph Configuration Template\n";
    file << "# Auto-generated based on FPGA image\n";
    file << "# Device: " << graph->get_tree()->access<std::string>("/name").get() << "\n";

    std::cout << "\nInside the chrono section scope" <<std::endl;
    time_t temp_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto test = std::put_time(std::gmtime(&temp_time), "%Y-%m-%d %H:%M:%S UTC");
    file << "# Generated: " << test << "\n\n";
    
    // Multi-stream configuration if multiple streaming endpoints found
    if (topology.block_stream_ports.size() > 1 || 
        (topology.block_stream_ports.size() == 1 && 
         topology.block_stream_ports.begin()->second.size() > 1)) {
        
        file << "# Multi-stream configuration (multiple streaming endpoints detected)\n";
        file << "multi_stream:\n";
        file << "  enable: true\n";
        file << "  sync_streams: true\n";
        file << "  separate_files: false\n";
        file << "  file_prefix: \"stream\"\n";
        file << "  max_streams: 0\n";
        file << "  sync_delay: 0.1\n";
        file << "  stream_blocks:\n";
        
        for (const auto& [block_id, ports] : topology.block_stream_ports) {
            for (const auto& port : ports) {
                file << "    - \"" << block_id << "\"  # Port " << port << "\n";
            }
        }
        file << "\n";
    }
    
    // Static connections section
    if (!topology.static_connections.empty()) {
        file << "# Static connections (hardcoded in FPGA image)\n";
        file << "# These connections cannot be changed at runtime\n";
        file << "static_connections:\n";
        for (const auto& edge : topology.static_connections) {
            file << "  # " << edge.src_blockid << ":" << edge.src_port 
                 << " -> " << edge.dst_blockid << ":" << edge.dst_port << "\n";
        }
        file << "\n";
    }
    
    // Dynamic connections based on discovered topology
    file << "# Dynamic block connections\n";
    file << "# Add/modify connections as needed for your application\n";
    file << "connections:\n";
    
    // Find potential dynamic connections
    std::set<std::string> connected_pairs;
    for (const auto& edge : topology.active_connections) {
        bool is_static = false;
        for (const auto& static_edge : topology.static_connections) {
            if (edge.src_blockid == static_edge.src_blockid &&
                edge.src_port == static_edge.src_port &&
                edge.dst_blockid == static_edge.dst_blockid &&
                edge.dst_port == static_edge.dst_port) {
                is_static = true;
                break;
            }
        }
        
        if (!is_static) {
            std::string pair_key = edge.src_blockid + ":" + std::to_string(edge.src_port) +
                                  "->" + edge.dst_blockid + ":" + std::to_string(edge.dst_port);
            if (connected_pairs.find(pair_key) == connected_pairs.end()) {
                file << "  - src_block: \"" << edge.src_blockid << "\"\n";
                file << "    src_port: " << edge.src_port << "\n";
                file << "    dst_block: \"" << edge.dst_blockid << "\"\n";
                file << "    dst_port: " << edge.dst_port << "\n";
                connected_pairs.insert(pair_key);
            }
        }
    }
    
    // Add example connections for unconnected ports
    file << "\n  # Example connections for available ports:\n";
    for (const auto& block : topology.blocks) {
        for (size_t out_port = 0; out_port < block.num_output_ports; ++out_port) {
            if (topology.downstream_connections[block.block_id].find(out_port) == 
                topology.downstream_connections[block.block_id].end()) {
                file << "  # " << block.block_id << ":" << out_port << " (output) is available\n";
            }
        }
        for (size_t in_port = 0; in_port < block.num_input_ports; ++in_port) {
            if (topology.upstream_connections[block.block_id].find(in_port) == 
                topology.upstream_connections[block.block_id].end()) {
                file << "  # " << block.block_id << ":" << in_port << " (input) is available\n";
            }
        }
    }
    
    // Switchboard configurations
    file << "\n# Switchboard routing configuration\n";
    file << "switchboards:\n";
    bool has_switchboard = false;
    for (const auto& block : topology.blocks) {
        if (block.block_type == "Switchboard") {
            has_switchboard = true;
            file << "  - block_id: \"" << block.block_id << "\"\n";
            file << "    connections:\n";
            
            // Add default passthrough connections
            size_t num_ports = std::min(block.num_input_ports, block.num_output_ports);
            for (size_t i = 0; i < num_ports; ++i) {
                file << "      - input: " << i << "\n";
                file << "        output: " << i << "\n";
            }
        }
    }
    if (!has_switchboard) {
        file << "  # No switchboard blocks detected in this image\n";
    }
    
    // Stream endpoints
    file << "\n# Stream endpoint configuration\n";
    file << "stream_endpoints:\n";
    
    int stream_idx = 0;
    for (const auto& [block_id, ports] : topology.block_stream_ports) {
        for (const auto& port : ports) {
            file << "  - block_id: \"" << block_id << "\"\n";
            file << "    port: " << port << "\n";
            file << "    direction: \"rx\"\n";
            file << "    enabled: true\n";
            file << "    name: \"Stream_" << stream_idx++ << "\"\n";
            file << "    stream_args:\n";
            file << "      spp: \"2000\"\n";
        }
    }
    
    // Block properties section
    file << "\n# Block-specific properties\n";
    file << "# Configure based on your application requirements\n";
    file << "block_properties:\n";
    
    // Group blocks by type for better organization
    std::map<std::string, std::vector<BlockInfo>> blocks_by_type;
    for (const auto& block : topology.blocks) {
        blocks_by_type[block.block_type].push_back(block);
    }
    
    // Write properties for each block type
    for (const auto& [block_type, blocks] : blocks_by_type) {
        file << "\n  # " << block_type << " block configuration\n";
        
        for (const auto& block : blocks) {
            file << "  \"" << block.block_id << "\":\n";
            
            auto properties = get_block_property_template(block_type, block, graph);
            
            if (properties.empty()) {
                file << "    # No configurable properties for this block type\n";
            } else {
                for (const auto& [prop, value] : properties) {
                    file << "    " << prop << ": \"" << value << "\"\n";
                }
            }
        }
    }
    
    // Auto-connect settings
    file << "\n# Auto-connection settings\n";
    file << "auto_connect:\n";
    
    // Check if Radio and DDC blocks exist
    bool has_radio = false, has_ddc = false;
    for (const auto& block : topology.blocks) {
        if (block.block_type == "Radio") has_radio = true;
        if (block.block_type == "DDC") has_ddc = true;
    }
    
    file << "  radio_to_ddc: " << (has_radio && has_ddc ? "true" : "false") 
         << "  # " << (has_radio && has_ddc ? "Radio and DDC blocks detected" : 
                       "Radio/DDC blocks not found") << "\n";
    file << "  find_stream_endpoint: " 
         << (topology.block_stream_ports.empty() ? "true" : "false") << "\n";
    
    // Advanced options
    file << "\n# Advanced configuration options\n";
    file << "advanced:\n";
    file << "  commit_after_each_connection: false\n";
    file << "  discover_static_connections: true\n";
    file << "  preserve_static_routes: true\n";
    file << "  block_init_order:\n";
    
    // Suggest initialization order based on topology
    std::vector<std::string> init_order;
    
    // First, Radio blocks
    for (const auto& block : topology.blocks) {
        if (block.block_type == "Radio") {
            init_order.push_back(block.block_id);
        }
    }
    
    // Then DDC/DUC blocks
    for (const auto& block : topology.blocks) {
        if (block.block_type == "DDC" || block.block_type == "DUC") {
            init_order.push_back(block.block_id);
        }
    }
    
    // Then other processing blocks
    for (const auto& block : topology.blocks) {
        if (block.block_type != "Radio" && 
            block.block_type != "DDC" && 
            block.block_type != "DUC") {
            init_order.push_back(block.block_id);
        }
    }
    
    for (const auto& block_id : init_order) {
        file << "    - \"" << block_id << "\"\n";
    }
    
    file.close();
    
    // Print summary
    std::cout << "\nYAML template generated: " << filename << std::endl;
    std::cout << "Summary of discovered hardware:" << std::endl;
    std::cout << "  Total blocks: " << topology.blocks.size() << std::endl;
    std::cout << "  Static connections: " << topology.static_connections.size() << std::endl;
    std::cout << "  Active connections: " << topology.active_connections.size() << std::endl;
    std::cout << "  Stream endpoints: " << topology.block_stream_ports.size() << " blocks with "
              << std::accumulate(topology.block_stream_ports.begin(), 
                                topology.block_stream_ports.end(), 0,
                                [](int sum, const auto& pair) { 
                                    return sum + pair.second.size(); 
                                }) << " total ports" << std::endl;
    
    // Print block types found
    std::cout << "\nBlock types in FPGA image:" << std::endl;
    for (const auto& [block_type, blocks] : blocks_by_type) {
        std::cout << "  " << block_type << ": " << blocks.size() << " instance(s)" << std::endl;
    }
}

// Enhanced function to detect actual streaming capabilities
std::vector<std::pair<std::string, size_t>> detect_stream_endpoints(
    uhd::rfnoc::rfnoc_graph::sptr graph) {
    
    std::vector<std::pair<std::string, size_t>> endpoints;
    auto blocks = discover_blocks(graph);
    
    for (const auto& block : blocks) {
        uhd::rfnoc::block_id_t block_id(block.block_id);
        auto noc_block = graph->get_block(block_id);
        
        // For DDC blocks, check each output port's connectivity
        if (block.block_type == "DDC") {
            auto ddc = std::dynamic_pointer_cast<uhd::rfnoc::ddc_block_control>(noc_block);
            if (ddc) {
                // DDCs can stream from any output port
                for (size_t port = 0; port < ddc->get_num_output_ports(); ++port) {
                    // Check if this port has downstream connections
                    bool has_downstream = false;
                    auto edges = graph->enumerate_active_connections();
                    for (const auto& edge : edges) {
                        if (edge.src_blockid == block.block_id && edge.src_port == port) {
                            has_downstream = true;
                            break;
                        }
                    }
                    
                    // If no downstream connection, it's a streaming endpoint
                    if (!has_downstream) {
                        endpoints.push_back(std::make_pair(block.block_id, port));
                    }
                }
            }
        }
        // Radio blocks can stream
        else if (block.block_type == "Radio") {
            auto radio = std::dynamic_pointer_cast<uhd::rfnoc::radio_control>(noc_block);
            if (radio) {
                // Radio blocks typically stream from output ports
                for (size_t port = 0; port < radio->get_num_output_ports(); ++port) {
                    endpoints.push_back(std::make_pair(block.block_id, port));
                }
            }
        }
        // DUC blocks can stream (for TX)
        else if (block.block_type == "DUC") {
            auto duc = std::dynamic_pointer_cast<uhd::rfnoc::duc_block_control>(noc_block);
            if (duc) {
                // DUCs stream to their input ports (TX direction)
                for (size_t port = 0; port < duc->get_num_input_ports(); ++port) {
                    endpoints.push_back(std::make_pair(block.block_id, port));
                }
            }
        }
        // Replay blocks can stream
        else if (block.block_type == "Replay") {
            auto replay = std::dynamic_pointer_cast<uhd::rfnoc::replay_block_control>(noc_block);
            if (replay) {
                // Replay blocks can stream from output ports
                for (size_t port = 0; port < replay->get_num_output_ports(); ++port) {
                    endpoints.push_back(std::make_pair(block.block_id, port));
                }
            }
        }
        // DMA FIFO blocks can stream
        else if (block.block_type == "DmaFIFO") {
            // DMA FIFOs typically have streaming capability
            for (size_t port = 0; port < block.num_output_ports; ++port) {
                endpoints.push_back(std::make_pair(block.block_id, port));
            }
        }
        // Signal Generator blocks can stream
        else if (block.block_type == "SigGen") {
            auto siggen = std::dynamic_pointer_cast<uhd::rfnoc::siggen_block_control>(noc_block);
            if (siggen) {
                for (size_t port = 0; port < siggen->get_num_output_ports(); ++port) {
                    endpoints.push_back(std::make_pair(block.block_id, port));
                }
            }
        }
        // Null source blocks can stream
        else if (block.block_type == "NullSrcSink") {
            // Check if it's configured as a source
            for (size_t port = 0; port < block.num_output_ports; ++port) {
                endpoints.push_back(std::make_pair(block.block_id, port));
            }
        }
    }
    
    return endpoints;
}


// Forward declarations
bool auto_connect_radio_to_ddc(uhd::rfnoc::rfnoc_graph::sptr graph);

// Check if a block type typically has streaming capability
bool is_potentially_streamable_block(const std::string& block_type) {
    // List of block types that typically support streaming
    static const std::set<std::string> streamable_types = {
        "Radio", "DDC", "DUC", "Replay", "DmaFIFO", "SigGen",
        "NullSrcSink", "StreamEndpoint", "SEP"
    };
    
    return streamable_types.count(block_type) > 0;
}

// Check if a specific block/port has a SEP (Stream Endpoint) downstream
bool has_sep_downstream(uhd::rfnoc::rfnoc_graph::sptr graph,
                       const std::string& block_id,
                       size_t port) {
    try {
        // Get all edges
        auto all_edges = graph->enumerate_active_connections();
        auto static_edges = graph->enumerate_static_connections();
        all_edges.insert(all_edges.end(), static_edges.begin(), static_edges.end());
        
        // Follow the chain downstream
        std::string current_block = block_id;
        size_t current_port = port;
        std::set<std::string> visited;
        
        while (true) {
            std::string key = current_block + ":" + std::to_string(current_port);
            if (visited.count(key) > 0) break;
            visited.insert(key);
            
            // Check if current block is a SEP
            if (current_block.find("SEP") != std::string::npos ||
                current_block.find("StreamEndpoint") != std::string::npos) {
                return true;
            }
            
            // Find next downstream block
            bool found = false;
            for (const auto& edge : all_edges) {
                if (edge.src_blockid == current_block && edge.src_port == current_port) {
                    current_block = edge.dst_blockid;
                    current_port = edge.dst_port;
                    found = true;
                    break;
                }
            }
            
            if (!found) break;
        }
        
        return false;
    } catch (...) {
        return false;
    }
}

// Check if a block/port combination can be used for streaming
bool is_streamable_endpoint(uhd::rfnoc::rfnoc_graph::sptr graph,
                           const std::string& block_id,
                           size_t port) {
    try {
        uhd::rfnoc::block_id_t bid(block_id);
        auto block = graph->get_block(bid);
        
        if (!block || port >= block->get_num_output_ports()) {
            return false;
        }
        
        // Check if this is a known streamable block type
        std::string block_type = bid.get_block_name();
        if (is_potentially_streamable_block(block_type)) {
            return true;
        }
        
        // Check if this block has a SEP downstream
        if (has_sep_downstream(graph, block_id, port)) {
            return true;
        }
        
        // For other blocks, check if they're terminal (no downstream connections)
        // and could potentially stream
        auto all_edges = graph->enumerate_active_connections();
        auto static_edges = graph->enumerate_static_connections();
        all_edges.insert(all_edges.end(), static_edges.begin(), static_edges.end());
        
        bool has_downstream = false;
        for (const auto& edge : all_edges) {
            if (edge.src_blockid == block_id && edge.src_port == port) {
                has_downstream = true;
                break;
            }
        }
        
        // If it's a terminal output, it might be streamable
        return !has_downstream;
        
    } catch (...) {
        return false;
    }
}

// Find the terminal block in a chain (the last block before a SEP)
std::pair<std::string, size_t> find_terminal_stream_endpoint(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const std::string& start_block_id,
    size_t start_port) {
    
    try {
        // First check if this block ID is already a SEP - if so, we need to find what connects TO it
        if (start_block_id.find("SEP") != std::string::npos) {
            // This is a SEP block, find what connects to it
            auto all_edges = graph->enumerate_static_connections();
            auto active_edges = graph->enumerate_active_connections();
            all_edges.insert(all_edges.end(), active_edges.begin(), active_edges.end());
            
            for (const auto& edge : all_edges) {
                if (edge.dst_blockid == start_block_id && edge.dst_port == start_port) {
                    // Found the block that connects to this SEP
                    return {edge.src_blockid, edge.src_port};
                }
            }
            // If we can't find what connects to the SEP, return error
            throw std::runtime_error("SEP block has no upstream connection");
        }
        
        uhd::rfnoc::block_id_t block_id(start_block_id);
        auto block = graph->get_block(block_id);
        
        if (!block || start_port >= block->get_num_output_ports()) {
            throw std::runtime_error("Invalid block or port");
        }
        
        // Get all connections (both static and active)
        auto all_edges = graph->enumerate_active_connections();
        auto static_edges = graph->enumerate_static_connections();
        all_edges.insert(all_edges.end(), static_edges.begin(), static_edges.end());
        
        // Follow the chain downstream to find the terminal block
        std::string current_block = start_block_id;
        size_t current_port = start_port;
        std::string last_valid_block = start_block_id;
        size_t last_valid_port = start_port;
        std::set<std::string> visited;  // Prevent infinite loops
        
        while (true) {
            std::string block_port_key = current_block + ":" + std::to_string(current_port);
            if (visited.count(block_port_key) > 0) {
                // Loop detected, return last valid position
                break;
            }
            visited.insert(block_port_key);
            
            bool found_downstream = false;
            for (const auto& edge : all_edges) {
                if (edge.src_blockid == current_block && edge.src_port == current_port) {
                    // Check if downstream is a SEP
                    if (edge.dst_blockid.find("SEP") != std::string::npos) {
                        // Current block connects to a SEP, so current block is the terminal
                        return {current_block, current_port};
                    }
                    
                    // Save current as last valid before moving downstream
                    last_valid_block = current_block;
                    last_valid_port = current_port;
                    
                    // Move to downstream block
                    current_block = edge.dst_blockid;
                    current_port = edge.dst_port;
                    found_downstream = true;
                    break;
                }
            }
            
            if (!found_downstream) {
                // No more downstream connections, this is terminal
                return {current_block, current_port};
            }
        }
        
        return {last_valid_block, last_valid_port};
        
    } catch (const std::exception& e) {
        // On error, return the original
        return {start_block_id, start_port};
    }
}

// Also update the find_all_stream_endpoints_comprehensive function to filter out SEPs:

std::vector<std::pair<std::string, size_t>> find_all_stream_endpoints_comprehensive(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const std::vector<StreamEndpointConfig>& configured_endpoints,
    const MultiStreamConfig& multi_config) {
    
    std::vector<std::pair<std::string, size_t>> endpoints;
    std::set<std::string> processed_endpoints;  // Avoid duplicates
    
    // Helper to add unique endpoints (excluding SEPs)
    auto add_unique_endpoint = [&](const std::string& block_id, size_t port) {
        // Never add SEP blocks directly
        if (block_id.find("SEP") != std::string::npos) {
            return false;
        }
        
        std::string key = block_id + ":" + std::to_string(port);
        if (processed_endpoints.count(key) == 0) {
            processed_endpoints.insert(key);
            endpoints.push_back({block_id, port});
            return true;
        }
        return false;
    };
    
    // First, process explicitly configured endpoints
    for (const auto& ep : configured_endpoints) {
        if (ep.enabled && ep.direction == "rx") {
            auto terminal = find_terminal_stream_endpoint(graph, ep.block_id, ep.port);
            add_unique_endpoint(terminal.first, terminal.second);
        }
    }
    
    // Process specifically requested stream blocks
    if (!multi_config.stream_blocks.empty()) {
        for (const auto& block_id : multi_config.stream_blocks) {
            try {
                // Skip if it's a SEP
                if (block_id.find("SEP") != std::string::npos) {
                    continue;
                }
                
                uhd::rfnoc::block_id_t id(block_id);
                auto block = graph->get_block(id);
                
                if (!block) {
                    std::cerr << "Warning: Configured stream block " << block_id 
                             << " not found" << std::endl;
                    continue;
                }
                
                // Check ALL output ports
                for (size_t port = 0; port < block->get_num_output_ports(); ++port) {
                    auto terminal = find_terminal_stream_endpoint(graph, block_id, port);
                    if (add_unique_endpoint(terminal.first, terminal.second)) {
                        if (multi_config.max_streams > 0 && 
                            endpoints.size() >= multi_config.max_streams) {
                            return endpoints;
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Warning: Error processing stream block " << block_id 
                         << ": " << e.what() << std::endl;
            }
        }
    }
    
    // Auto-discover ALL potential streaming endpoints if enabled
    if (multi_config.enable_multi_stream && 
        (endpoints.empty() || (multi_config.max_streams > 0 && endpoints.size() < multi_config.max_streams))) {
        
        // Get all blocks
        auto block_ids = graph->find_blocks("");
        
        // Get all connections to find blocks that connect to SEPs
        auto all_edges = graph->enumerate_active_connections();
        auto static_edges = graph->enumerate_static_connections();
        all_edges.insert(all_edges.end(), static_edges.begin(), static_edges.end());
        
        // Find all blocks that have connections to SEPs
        for (const auto& edge : all_edges) {
            if (edge.dst_blockid.find("SEP") != std::string::npos) {
                // This edge connects to a SEP, so the source is streamable
                if (add_unique_endpoint(edge.src_blockid, edge.src_port)) {
                    std::cout << "Found streamable endpoint: " << edge.src_blockid 
                             << ":" << edge.src_port 
                             << " (connects to " << edge.dst_blockid << ")" << std::endl;
                    
                    if (multi_config.max_streams > 0 && 
                        endpoints.size() >= multi_config.max_streams) {
                        return endpoints;
                    }
                }
            }
        }
        
        // Also check for blocks that are known to be streamable even without SEPs
        for (const auto& block_id : block_ids) {
            try {
                auto block = graph->get_block(block_id);
                if (!block) continue;
                
                std::string block_id_str = block_id.to_string();
                
                // Skip SEP blocks
                if (block_id_str.find("SEP") != std::string::npos) {
                    continue;
                }
                
                // Check if it's a known streamable type without SEP
                if (is_potentially_streamable_block(block_id.get_block_name())) {
                    for (size_t port = 0; port < block->get_num_output_ports(); ++port) {
                        // Check if this port is already added or connects to a SEP
                        bool already_handled = false;
                        for (const auto& edge : all_edges) {
                            if (edge.src_blockid == block_id_str && 
                                edge.src_port == port &&
                                edge.dst_blockid.find("SEP") != std::string::npos) {
                                already_handled = true;
                                break;
                            }
                        }
                        
                        if (!already_handled) {
                            if (add_unique_endpoint(block_id_str, port)) {
                                std::cout << "Found additional streamable endpoint: " 
                                         << block_id_str << ":" << port << std::endl;
                                
                                if (multi_config.max_streams > 0 && 
                                    endpoints.size() >= multi_config.max_streams) {
                                    return endpoints;
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                // Skip blocks that cause errors
                continue;
            }
        }
    }
    
    return endpoints;
}

// Write value as little-endian bytes
template<typename T>
void write_le(std::vector<uint8_t>& buffer, T value) {
    for (size_t i = 0; i < sizeof(T); i++) {
        buffer.push_back((value >> (i * 8)) & 0xFF);
    }
}

// Read value from little-endian bytes
template<typename T>
T read_le(const uint8_t* data) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); i++) {
        value |= static_cast<T>(data[i]) << (i * 8);
    }
    return value;
}

// Automatically connect every Radio output to the matching DDC input
bool auto_connect_radio_to_ddc(uhd::rfnoc::rfnoc_graph::sptr graph)
{
    using uhd::rfnoc::block_id_t;
    bool   made_connection = false;

    const auto radio_blocks = graph->find_blocks("Radio");
    const auto ddc_blocks   = graph->find_blocks("DDC");

    if (radio_blocks.empty() || ddc_blocks.empty()) {
        std::cerr << "[auto-connect] No Radio or DDC blocks in this image – "
                  << "nothing to wire\n";
        return false;
    }

    const auto edge_exists = [&](const block_id_t& src, size_t s_port,
                                 const block_id_t& dst, size_t d_port) {
        for (const auto& e : graph->enumerate_active_connections()) {
            if (e.src_blockid == src.to_string() && e.src_port == s_port &&
                e.dst_blockid == dst.to_string() && e.dst_port == d_port) {
                return true;
            }
        }
        return false;
    };

    // Optional DSP blocks that can be in the chain
    const std::vector<std::string> optional_chain = {
        "Switchboard", "FIR", "Window", "FFT",
        "MovingAverage", "VectorIIR", "KeepOneInN"
    };

    for (const auto& radio_id : radio_blocks) {
        const size_t dev  = radio_id.get_device_no();
        const size_t chan = radio_id.get_block_count();

        block_id_t ddc_match;
        for (const auto& ddc_id : ddc_blocks) {
            if (ddc_id.get_device_no() == dev &&
                ddc_id.get_block_count() == chan)
            {
                ddc_match = ddc_id;
                break;
            }
        }
        if (!graph->has_block(ddc_match)) {
            continue; // no counterpart – skip this Radio
        }

        // Build the chain Radio → [...DSP...] → DDC
        block_id_t prev_blk = radio_id;
        size_t     prev_p   = 0; // always use port 0 for standard blocks

        // Scan optional single-port DSP blocks
        for (const auto& blk_name : optional_chain) {
            block_id_t candidate(dev, blk_name, chan);
            if (!graph->has_block(candidate)) {
                continue;
            }
            if (edge_exists(prev_blk, prev_p, candidate, 0)) {
                prev_blk = candidate;
                continue;
            }
            if (!graph->is_connectable(prev_blk, prev_p, candidate, 0)) {
                break;
            }
            graph->connect(prev_blk, prev_p, candidate, 0);
            prev_blk = candidate;
            made_connection = true;
        }

        // Finally connect to the DDC
        if (!edge_exists(prev_blk, prev_p, ddc_match, 0) &&
            graph->is_connectable(prev_blk, prev_p, ddc_match, 0))
        {
            graph->connect(prev_blk, prev_p, ddc_match, 0);
            made_connection = true;
        }
    }

    if (!made_connection) {
        std::cerr << "[auto-connect] No new Radio→DDC paths were created "
                  << "(everything was already wired or not connectable)\n";
    }
    return made_connection;
}

// Enhanced YAML configuration loader with multi-stream support
GraphConfig load_graph_config(const std::string& yaml_file) {
    GraphConfig config;
    
    try {
        YAML::Node root = YAML::LoadFile(yaml_file);
        
        // Load dynamic connections
        if (root["connections"]) {
            for (const auto& conn : root["connections"]) {
                ConnectionConfig cc;
                cc.src_block = conn["src_block"].as<std::string>();
                cc.src_port = conn["src_port"].as<size_t>(0);
                cc.dst_block = conn["dst_block"].as<std::string>();
                cc.dst_port = conn["dst_port"].as<size_t>(0);
                cc.is_back_edge = conn["is_back_edge"].as<bool>(false);
                config.dynamic_connections.push_back(cc);
            }
        }
        
        // Load switchboard configurations
        if (root["switchboards"]) {
            for (const auto& sb : root["switchboards"]) {
                SwitchboardConfig sbc;
                sbc.block_id = sb["block_id"].as<std::string>();
                if (sb["connections"]) {
                    for (const auto& conn : sb["connections"]) {
                        size_t input = conn["input"].as<size_t>();
                        size_t output = conn["output"].as<size_t>();
                        sbc.connections[input] = output;
                    }
                }
                config.switchboard_configs.push_back(sbc);
            }
        }
        
        // Load stream endpoint configurations - enhanced for multi-stream
        if (root["stream_endpoints"]) {
            for (const auto& sep : root["stream_endpoints"]) {
                StreamEndpointConfig sec;
                sec.block_id = sep["block_id"].as<std::string>();
                sec.port = sep["port"].as<size_t>(0);
                sec.direction = sep["direction"].as<std::string>("rx");
                sec.enabled = sep["enabled"].as<bool>(true);
                sec.stream_name = sep["name"].as<std::string>("");
                if (sep["stream_args"]) {
                    for (const auto& arg : sep["stream_args"]) {
                        sec.stream_args[arg.first.as<std::string>()] = arg.second.as<std::string>();
                    }
                }
                config.stream_endpoints.push_back(sec);
            }
        }
        
        // Load multi-stream configuration
        if (root["multi_stream"]) {
            auto& ms = config.multi_stream;
            ms.enable_multi_stream = root["multi_stream"]["enable"].as<bool>(false);
            ms.sync_streams = root["multi_stream"]["sync_streams"].as<bool>(true);
            ms.separate_files = root["multi_stream"]["separate_files"].as<bool>(false);
            ms.file_prefix = root["multi_stream"]["file_prefix"].as<std::string>("stream");
            ms.max_streams = root["multi_stream"]["max_streams"].as<size_t>(0);
            ms.sync_delay = root["multi_stream"]["sync_delay"].as<double>(0.1);
            
            if (root["multi_stream"]["stream_blocks"]) {
                for (const auto& block : root["multi_stream"]["stream_blocks"]) {
                    ms.stream_blocks.push_back(block.as<std::string>());
                }
            }
        }
        
        // Load block properties
        if (root["block_properties"]) {
            for (const auto& block : root["block_properties"]) {
                std::string block_id = block.first.as<std::string>();
                for (const auto& prop : block.second) {
                    std::string prop_name = prop.first.as<std::string>();
                    std::string prop_value = prop.second.as<std::string>();
                    config.block_properties[block_id][prop_name] = prop_value;
                }
            }
        }
        
        // Load auto-connect settings
        if (root["auto_connect"]) {
            config.auto_connect_radio_to_ddc = root["auto_connect"]["radio_to_ddc"].as<bool>(false);
            config.auto_find_stream_endpoint = root["auto_connect"]["find_stream_endpoint"].as<bool>(false);
        }
        
        // Load advanced options
        if (root["advanced"]) {
            config.commit_after_each_connection = root["advanced"]["commit_after_each_connection"].as<bool>(false);
            config.discover_static_connections = root["advanced"]["discover_static_connections"].as<bool>(true);
            config.preserve_static_routes = root["advanced"]["preserve_static_routes"].as<bool>(true);
            
            if (root["advanced"]["block_init_order"]) {
                for (const auto& block : root["advanced"]["block_init_order"]) {
                    config.block_init_order.push_back(block.as<std::string>());
                }
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading YAML config: " << e.what() << std::endl;
        // Return default config on error
    }
    
    return config;
}


// Find ALL available streaming endpoints for multi-stream support
/*std::vector<std::pair<std::string, size_t>> find_all_stream_endpoints_enhanced(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const std::vector<StreamEndpointConfig>& configured_endpoints,
    const MultiStreamConfig& multi_config) {
    
    std::vector<std::pair<std::string, size_t>> endpoints;
    
    // First, check explicitly configured endpoints
    for (const auto& ep : configured_endpoints) {
        if (ep.enabled && ep.direction == "rx") {
            // For each configured endpoint, find the actual last block in chain
            try {
                uhd::rfnoc::block_id_t block_id(ep.block_id);
                if (graph->has_block(block_id)) {
                    // Get the block chain starting from this block
                    auto edges = uhd::rfnoc::get_block_chain(graph, block_id, ep.port, true);
                    if (!edges.empty()) {
                        // The last edge's source is what we can actually connect to
                        endpoints.push_back({edges.back().src_blockid, edges.back().src_port});
                    } else {
                        // No chain, so this block itself is the endpoint
                        endpoints.push_back({ep.block_id, ep.port});
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Warning: Could not process endpoint " << ep.block_id 
                         << ": " << e.what() << std::endl;
            }
        }
    }
    
    // If we have specific stream blocks configured, process those
    if (!multi_config.stream_blocks.empty()) {
        for (const auto& block_id_str : multi_config.stream_blocks) {
            try {
                uhd::rfnoc::block_id_t block_id(block_id_str);
                if (!graph->has_block(block_id)) {
                    std::cerr << "Warning: Configured stream block " << block_id_str 
                             << " not found" << std::endl;
                    continue;
                }
                
                auto block = graph->get_block(block_id);
                size_t num_ports = block->get_num_output_ports();
                
                // Check each output port
                for (size_t port = 0; port < num_ports; ++port) {
                    // Get the block chain from this port
                    auto edges = uhd::rfnoc::get_block_chain(graph, block_id, port, true);
                    
                    std::string actual_block_id = block_id_str;
                    size_t actual_port = port;
                    
                    if (!edges.empty()) {
                        // Use the last block in the chain
                        actual_block_id = edges.back().src_blockid;
                        actual_port = edges.back().src_port;
                    }
                    
                    // Check if already added
                    bool already_added = false;
                    for (const auto& [bid, p] : endpoints) {
                        if (bid == actual_block_id && p == actual_port) {
                            already_added = true;
                            break;
                        }
                    }
                    
                    if (!already_added) {
                        endpoints.push_back({actual_block_id, actual_port});
                        
                        // Check if we've reached max_streams
                        if (multi_config.max_streams > 0 && 
                            endpoints.size() >= multi_config.max_streams) {
                            return endpoints;
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Warning: Error processing stream block " << block_id_str 
                         << ": " << e.what() << std::endl;
            }
        }
    }
    
    // Auto-discover if needed
    if (multi_config.enable_multi_stream && endpoints.empty()) {
        // Find all blocks that could be endpoints
        auto blocks = discover_blocks_enhanced(graph);
        
        for (const auto& block : blocks) {
            if (block.block_type == "Radio" || block.block_type == "DDC" || 
                block.block_type == "DUC" || block.block_type == "Replay" ||
                block.block_type == "DmaFIFO" || block.block_type == "SigGen") {
                
                for (size_t port = 0; port < block.num_output_ports; ++port) {
                    try {
                        // Get the chain from this block/port
                        uhd::rfnoc::block_id_t bid(block.block_id);
                        auto edges = uhd::rfnoc::get_block_chain(graph, bid, port, true);
                        
                        if (!edges.empty()) {
                            endpoints.push_back({edges.back().src_blockid, edges.back().src_port});
                        } else {
                            endpoints.push_back({block.block_id, port});
                        }
                        
                        if (multi_config.max_streams > 0 && 
                            endpoints.size() >= multi_config.max_streams) {
                            return endpoints;
                        }
                    } catch (...) {
                        // Skip ports that can't be used
                    }
                }
            }
        }
    }
    
    return endpoints;
}
*/

/*// Apply block properties for all RFNoC block types
bool apply_block_properties(uhd::rfnoc::rfnoc_graph::sptr graph,
                           const std::map<std::string, std::map<std::string, std::string>>& properties,
                           double default_rate = 1e6) {
    for (const auto& [block_id, props] : properties) {
        try {
            uhd::rfnoc::block_id_t id(block_id);
            auto block = graph->get_block(id);
            
            if (!block) {
                std::cerr << "Block " << block_id << " not found" << std::endl;
                continue;
            }
            
            std::cout << "Setting properties for " << block_id << std::endl;
            
            // Handle Radio block properties
            if (id.get_block_name() == "Radio") {
                auto radio = std::dynamic_pointer_cast<uhd::rfnoc::radio_control>(block);
                if (radio) {
                    for (const auto& [prop, value] : props) {
                        size_t chan = 0;
                        // Extract channel from property name if present (e.g., "freq/0")
                        std::regex chan_regex("(.+)/(\\d+)");
                        std::smatch match;
                        std::string prop_name = prop;
                        if (std::regex_match(prop, match, chan_regex)) {
                            prop_name = match[1];
                            chan = std::stoul(match[2]);
                        }
                        
                        if (prop_name == "freq" || prop_name == "rx_freq") {
                            radio->set_rx_frequency(std::stod(value), chan);
                            std::cout << "  Set RX frequency[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "tx_freq") {
                            radio->set_tx_frequency(std::stod(value), chan);
                            std::cout << "  Set TX frequency[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "gain" || prop_name == "rx_gain") {
                            radio->set_rx_gain(std::stod(value), chan);
                            std::cout << "  Set RX gain[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "tx_gain") {
                            radio->set_tx_gain(std::stod(value), chan);
                            std::cout << "  Set TX gain[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "antenna" || prop_name == "rx_antenna") {
                            radio->set_rx_antenna(value, chan);
                            std::cout << "  Set RX antenna[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "tx_antenna") {
                            radio->set_tx_antenna(value, chan);
                            std::cout << "  Set TX antenna[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "rate") {
                            radio->set_rate(std::stod(value));
                            std::cout << "  Set rate: " << value << std::endl;
                        } else if (prop_name == "bandwidth" || prop_name == "rx_bandwidth") {
                            radio->set_rx_bandwidth(std::stod(value), chan);
                            std::cout << "  Set RX bandwidth[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "tx_bandwidth") {
                            radio->set_tx_bandwidth(std::stod(value), chan);
                            std::cout << "  Set TX bandwidth[" << chan << "]: " << value << std::endl;
                        }
                    }
                }
            }
            // Handle DDC block properties
            else if (id.get_block_name() == "DDC") {
                auto ddc = std::dynamic_pointer_cast<uhd::rfnoc::ddc_block_control>(block);
                if (ddc) {
                    for (const auto& [prop, value] : props) {
                        size_t chan = 0;
                        std::regex chan_regex("(.+)/(\\d+)");
                        std::smatch match;
                        std::string prop_name = prop;
                        if (std::regex_match(prop, match, chan_regex)) {
                            prop_name = match[1];
                            chan = std::stoul(match[2]);
                        }
                        
                        if (prop_name == "freq") {
                            ddc->set_freq(std::stod(value), chan);
                            std::cout << "  Set DDC frequency[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "output_rate") {
                            ddc->set_output_rate(std::stod(value), chan);
                            std::cout << "  Set output rate[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "input_rate") {
                            ddc->set_input_rate(std::stod(value), chan);
                            std::cout << "  Set input rate[" << chan << "]: " << value << std::endl;
                        }
                    }
                }
            }
            // ... [Other block types remain the same] ...
            else if (id.get_block_name() == "SigGen") {
                auto siggen = std::dynamic_pointer_cast<uhd::rfnoc::siggen_block_control>(block);
                if (siggen) {
                    for (const auto& [prop, value] : props) {
                        size_t chan = 0;
                        std::regex chan_regex("(.+)/(\\d+)");
                        std::smatch match;
                        std::string prop_name = prop;
                        if (std::regex_match(prop, match, chan_regex)) {
                            prop_name = match[1];
                            chan = std::stoul(match[2]);
                        }
                        
                        if (prop_name == "enable") {
                            bool enable = (value == "true" || value == "1");
                            siggen->set_enable(enable, chan);
                            std::cout << "  Set enable[" << chan << "]: " << (enable ? "true" : "false") << std::endl;
                        } else if (prop_name == "waveform") {
                            // Set waveform type
                            if (value == "sine" || value == "SINE") {
                                siggen->set_waveform(uhd::rfnoc::siggen_waveform::SINE_WAVE, chan);
                            } else if (value == "constant" || value == "CONSTANT") {
                                siggen->set_waveform(uhd::rfnoc::siggen_waveform::CONSTANT, chan);
                            } else if (value == "noise" || value == "NOISE") {
                                siggen->set_waveform(uhd::rfnoc::siggen_waveform::NOISE, chan);
                            }
                            std::cout << "  Set waveform[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "amplitude") {
                            siggen->set_amplitude(std::stod(value), chan);
                            std::cout << "  Set amplitude[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "frequency") {
                            // Frequency is specified in Hz, need to convert to phase increment
                            double freq_hz = std::stod(value);
                            double sample_rate = 200e6;  // FPGA clock rate
                            // phase_inc = freq * 2^32 / sample_rate
                            uint32_t phase_inc = static_cast<uint32_t>((freq_hz * 4294967296.0) / sample_rate);
                            siggen->set_sine_phase_increment(phase_inc, chan);
                            std::cout << "  Set frequency[" << chan << "]: " << value << " Hz (phase_inc: " << phase_inc << ")" << std::endl;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to set properties for " << block_id << ": " << e.what() << std::endl;
            return false;
        }
    }
    return true;
}*/

// Configure switchboards from configuration
bool configure_switchboards(uhd::rfnoc::rfnoc_graph::sptr graph,
                           const std::vector<SwitchboardConfig>& configs) {
    for (const auto& config : configs) {
        try {
            uhd::rfnoc::block_id_t block_id(config.block_id);
            auto switchboard = graph->get_block<uhd::rfnoc::switchboard_block_control>(block_id);
            
            if (!switchboard) {
                std::cerr << "Switchboard " << config.block_id << " not found" << std::endl;
                continue;
            }
            
            std::cout << "Configuring switchboard " << config.block_id << std::endl;
            
            for (const auto& [input, output] : config.connections) {
                switchboard->connect(input, output);
                std::cout << "  Connected input " << input << " to output " << output << std::endl;
            }
        // std::cout << "\nJust to see what the connections are right now after configuring " << config.block_id << std::endl;
        
        // graph->commit();

        // std::cout << "\nActive connections:" << std::endl;
        // auto active_edges = graph->enumerate_active_connections();
        // for (const auto& edge : active_edges) {
        //     std::cout << "  * " << edge.to_string() << std::endl;
        // }

        } catch (const uhd::exception& e) {
            std::cerr << "Failed to configure switchboard: " << e.what() << std::endl;
            return false;
        }
    }
    
    

    return true;
}

// Apply dynamic connections with optional per-connection commit
bool apply_dynamic_connections(uhd::rfnoc::rfnoc_graph::sptr graph,
                              const std::vector<ConnectionConfig>& connections,
                              bool commit_after_each = false) {
    for (const auto& conn : connections) {
        try {
            std::cout << "Connecting " << conn.src_block << ":" << conn.src_port
                     << " -> " << conn.dst_block << ":" << conn.dst_port;
            if (conn.is_back_edge) {
                std::cout << " (back edge)";
            }
            std::cout << std::endl;
            
            if (conn.is_back_edge) {
                graph->connect(conn.src_block, conn.src_port, 
                              conn.dst_block, conn.dst_port, true);
            } else {
                graph->connect(conn.src_block, conn.src_port, 
                              conn.dst_block, conn.dst_port);
            }
            
            if (commit_after_each) {
                graph->commit();
            }
        } catch (const uhd::exception& e) {
            std::cerr << "Failed to connect: " << e.what() << std::endl;
            return false;
        }
    }
    return true;
}

// Print enhanced graph information
void print_graph_info(uhd::rfnoc::rfnoc_graph::sptr graph) {
    std::cout << "\n=== RFNoC Graph Information ===" << std::endl;
    
    // Print blocks with more details
    auto blocks = discover_blocks(graph);
    std::cout << "\nRFNoC blocks:" << std::endl;
    for (const auto& block : blocks) {
        std::cout << "  * " << block.block_id 
                  << " (Type: " << block.block_type
                  << ", In: " << block.num_input_ports
                  << ", Out: " << block.num_output_ports;
        if (block.has_stream_endpoint) {
            std::cout << ", SEP";
        }
        std::cout << ")" << std::endl;
        
        // Print available properties
        if (!block.properties.empty()) {
            std::cout << "    Properties: ";
            for (size_t i = 0; i < block.properties.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << block.properties[i];
            }
            std::cout << std::endl;
        }
    }
    
    // Print static connections
    std::cout << "\nStatic connections:" << std::endl;
    auto static_edges = graph->enumerate_static_connections();
    for (const auto& edge : static_edges) {
        std::cout << "  * " << edge.to_string() << std::endl;
    }
    
    // Print active connections
    std::cout << "\nActive connections:" << std::endl;
    auto active_edges = graph->enumerate_active_connections();
    for (const auto& edge : active_edges) {
        std::cout << "  * " << edge.to_string() << std::endl;
    }
}

// Write enhanced YAML config template with multi-stream support
void write_yaml_template(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to create YAML template file");
    }
    
    file << "# RFNoC Graph Configuration Template - Multi-Stream Enhanced\n";
    file << "# Generated by CHDR Capture Tool\n\n";
    
    file << "# Multi-stream configuration\n";
    file << "multi_stream:\n";
    file << "  enable: true              # Enable multi-stream capture\n";
    file << "  sync_streams: true        # Synchronize stream start times\n";
    file << "  separate_files: false     # Write each stream to separate file\n";
    file << "  file_prefix: \"stream\"     # Prefix for separate files\n";
    file << "  max_streams: 0            # Maximum streams (0 = unlimited)\n";
    file << "  sync_delay: 0.1           # Delay before synchronized start (seconds)\n";
    file << "  stream_blocks:            # Specific blocks to stream from\n";
    file << "    - \"0/DDC#0\"\n";
    file << "    - \"0/DDC#1\"\n";
    file << "    - \"0/DDC#2\"\n";
    file << "    - \"0/DDC#3\"\n\n";
    
    file << "# Dynamic block connections\n";
    file << "connections:\n";
    file << "  # Example: Connect multiple Radio channels to DDCs\n";
    file << "  - src_block: \"0/Radio#0\"\n";
    file << "    src_port: 0\n";
    file << "    dst_block: \"0/DDC#0\"\n";
    file << "    dst_port: 0\n";
    file << "  - src_block: \"0/Radio#0\"\n";
    file << "    src_port: 1\n";
    file << "    dst_block: \"0/DDC#1\"\n";
    file << "    dst_port: 0\n";
    file << "  - src_block: \"0/Radio#1\"\n";
    file << "    src_port: 0\n";
    file << "    dst_block: \"0/DDC#2\"\n";
    file << "    dst_port: 0\n";
    file << "  - src_block: \"0/Radio#1\"\n";
    file << "    src_port: 1\n";
    file << "    dst_block: \"0/DDC#3\"\n";
    file << "    dst_port: 0\n\n";
    
    file << "# Stream endpoint configuration - enhanced for multi-stream\n";
    file << "stream_endpoints:\n";
    file << "  - block_id: \"0/DDC#0\"\n";
    file << "    port: 0\n";
    file << "    direction: \"rx\"\n";
    file << "    enabled: true\n";
    file << "    name: \"Channel_0\"\n";
    file << "    stream_args:\n";
    file << "      spp: \"2000\"\n";
    file << "  - block_id: \"0/DDC#1\"\n";
    file << "    port: 0\n";
    file << "    direction: \"rx\"\n";
    file << "    enabled: true\n";
    file << "    name: \"Channel_1\"\n";
    file << "    stream_args:\n";
    file << "      spp: \"2000\"\n";
    file << "  - block_id: \"0/DDC#2\"\n";
    file << "    port: 0\n";
    file << "    direction: \"rx\"\n";
    file << "    enabled: true\n";
    file << "    name: \"Channel_2\"\n";
    file << "    stream_args:\n";
    file << "      spp: \"2000\"\n";
    file << "  - block_id: \"0/DDC#3\"\n";
    file << "    port: 0\n";
    file << "    direction: \"rx\"\n";
    file << "    enabled: true\n";
    file << "    name: \"Channel_3\"\n";
    file << "    stream_args:\n";
    file << "      spp: \"2000\"\n\n";
    
    file << "# Block-specific properties for multi-channel setup\n";
    file << "block_properties:\n";
    file << "  # Radio block configuration - multi-channel\n";
    file << "  \"0/Radio#0\":\n";
    file << "    rate: \"200e6\"         # Master clock rate\n";
    file << "    freq/0: \"1e9\"         # Channel 0 frequency\n";
    file << "    gain/0: \"30\"          # Channel 0 gain\n";
    file << "    antenna/0: \"RX2\"      # Channel 0 antenna\n";
    file << "    freq/1: \"1.1e9\"       # Channel 1 frequency\n";
    file << "    gain/1: \"30\"          # Channel 1 gain\n";
    file << "    antenna/1: \"RX2\"      # Channel 1 antenna\n";
    file << "  \"0/Radio#1\":\n";
    file << "    rate: \"200e6\"\n";
    file << "    freq/0: \"1.2e9\"\n";
    file << "    gain/0: \"30\"\n";
    file << "    antenna/0: \"RX2\"\n";
    file << "    freq/1: \"1.3e9\"\n";
    file << "    gain/1: \"30\"\n";
    file << "    antenna/1: \"RX2\"\n\n";
    
    file << "  # DDC configuration for each channel\n";
    file << "  \"0/DDC#0\":\n";
    file << "    freq: \"0\"            # No frequency offset\n";
    file << "    output_rate: \"1e6\"   # 1 Msps output\n";
    file << "  \"0/DDC#1\":\n";
    file << "    freq: \"100e3\"        # 100 kHz offset\n";
    file << "    output_rate: \"1e6\"\n";
    file << "  \"0/DDC#2\":\n";
    file << "    freq: \"-100e3\"       # -100 kHz offset\n";
    file << "    output_rate: \"1e6\"\n";
    file << "  \"0/DDC#3\":\n";
    file << "    freq: \"0\"\n";
    file << "    output_rate: \"1e6\"\n\n";
    
    file << "# Auto-connection settings\n";
    file << "auto_connect:\n";
    file << "  radio_to_ddc: false     # Disable auto-connect for manual control\n";
    file << "  find_stream_endpoint: false  # Use explicit endpoints\n\n";
    
    file << "# Advanced configuration options\n";
    file << "advanced:\n";
    file << "  commit_after_each_connection: false\n";
    file << "  discover_static_connections: true\n";
    file << "  preserve_static_routes: true\n";
    file << "  block_init_order:\n";
    file << "    - \"0/Radio#0\"\n";
    file << "    - \"0/Radio#1\"\n";
    file << "    - \"0/DDC#0\"\n";
    file << "    - \"0/DDC#1\"\n";
    file << "    - \"0/DDC#2\"\n";
    file << "    - \"0/DDC#3\"\n";
    
    file.close();
    std::cout << "YAML template written to: " << filename << std::endl;
}

// Analyze packets with multi-stream support
void analyze_packets(const std::vector<chdr_packet_data>& packets, 
                    const std::string& csv_file,
                    double tick_rate,
                    const std::vector<StreamStats>& stream_stats) {
    std::ofstream csv(csv_file);
    if (!csv.is_open()) {
        throw std::runtime_error("Failed to open CSV file: " + csv_file);
    }
    
    // Write CSV header with stream info
    csv << "packet_num,stream_id,stream_block,stream_port,vc,eob,eov,pkt_type,pkt_type_str,"
        << "num_mdata,seq_num,length,dst_epid,has_timestamp,timestamp_ticks,"
        << "timestamp_sec,payload_size,num_samples,first_4_bytes_hex" << std::endl;
    
    // Analyze each packet
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& pkt = packets[i];
        
        // Stream info
        csv << i << ","
            << pkt.stream_id << ","
            << "\"" << pkt.stream_block << "\","
            << pkt.stream_port << ",";
        
        // Basic fields
        csv << std::hex << "0x" << std::setw(2) << std::setfill('0') << (int)pkt.vc << ","
            << std::dec << (pkt.eob ? "1" : "0") << ","
            << (pkt.eov ? "1" : "0") << ","
            << std::hex << "0x" << (int)pkt.pkt_type << ","
            << pkt.pkt_type_str() << ","
            << std::dec << (int)pkt.num_mdata << ","
            << pkt.seq_num << ","
            << pkt.length << ","
            << std::hex << "0x" << std::setw(4) << std::setfill('0') << pkt.dst_epid << ",";
        
        // Timestamp
        csv << (pkt.has_timestamp ? "1" : "0") << ",";
        if (pkt.has_timestamp) {
            double timestamp_sec = static_cast<double>(pkt.timestamp) / tick_rate;
            csv << std::dec << pkt.timestamp << ","
                << std::fixed << std::setprecision(12) << timestamp_sec;
        } else {
            csv << "N/A,N/A";
        }
        
        // Payload info
        size_t num_samples = pkt.payload.size() / 4; // Assuming sc16
        csv << "," << std::dec << pkt.payload.size() << ","
            << num_samples << ",";
        
        // First few bytes as hex
        csv << "0x";
        for (size_t j = 0; j < std::min((size_t)4, pkt.payload.size()); j++) {
            csv << std::hex << std::setw(2) << std::setfill('0') 
                << (unsigned int)pkt.payload[j];
        }
        
        csv << std::endl;
    }
    
    csv.close();
    
    // Write stream statistics summary
    std::string stats_file = csv_file.substr(0, csv_file.find_last_of('.')) + "_stats.csv";
    std::ofstream stats_csv(stats_file);
    if (stats_csv.is_open()) {
        stats_csv << "stream_id,block_id,port,packets_captured,total_samples,"
                  << "overflow_count,error_count,duration_sec,avg_sample_rate_msps,"
                  << "first_timestamp,last_timestamp" << std::endl;
        
        for (const auto& stat : stream_stats) {
            double duration = std::chrono::duration<double>(stat.end_time - stat.start_time).count();
            double avg_rate = (stat.total_samples / duration) / 1e6;
            
            stats_csv << stat.stream_id << ","
                     << "\"" << stat.block_id << "\","
                     << stat.port << ","
                     << stat.packets_captured << ","
                     << stat.total_samples << ","
                     << stat.overflow_count << ","
                     << stat.error_count << ","
                     << std::fixed << std::setprecision(6) << duration << ","
                     << avg_rate << ","
                     << std::setprecision(12) << stat.first_timestamp << ","
                     << stat.last_timestamp << std::endl;
        }
        stats_csv.close();
        std::cout << "Stream statistics written to: " << stats_file << std::endl;
    }
    
    std::cout << "Analysis complete. Results written to: " << csv_file << std::endl;
}

// Enhanced connection establishment function
void establish_signal_paths(uhd::rfnoc::rfnoc_graph::sptr graph,
                          const GraphConfig& config,
                          const std::vector<std::pair<std::string, size_t>>& endpoints) {
    
    std::cout << "\nEstablishing signal paths..." << std::endl;
    
    // Process each endpoint to ensure complete path from source
    for (const auto& [endpoint_block, endpoint_port] : endpoints) {
        uhd::rfnoc::block_id_t endpoint_id(endpoint_block);
        
        // Only process DDC endpoints for now (extend as needed)
        if (endpoint_id.get_block_name() != "DDC") {
            continue;
        }
        
        // Build the complete path based on static connections
        auto static_conns = graph->enumerate_static_connections();
        std::vector<uhd::rfnoc::graph_edge_t> path;
        
        // Trace backwards from endpoint to find source
        std::string current_block = endpoint_block;
        size_t current_port = endpoint_port;
        std::set<std::string> visited;  // Prevent loops
        
        while (true) {
            std::string key = current_block + ":" + std::to_string(current_port);
            if (visited.count(key) > 0) break;
            visited.insert(key);
            
            bool found_upstream = false;
            for (const auto& conn : static_conns) {
                if (conn.dst_blockid == current_block && conn.dst_port == current_port) {
                    path.push_back(conn);
                    current_block = conn.src_blockid;
                    current_port = conn.src_port;
                    found_upstream = true;
                    break;
                }
            }
            
            if (!found_upstream) break;
            
            // Stop at Radio (our typical source)
            uhd::rfnoc::block_id_t current_id(current_block);
            if (current_id.get_block_name() == "Radio") {
                break;
            }
        }
        
        // Now establish connections forward along the path
        if (!path.empty()) {
            std::reverse(path.begin(), path.end());  // Make it source->destination order
            
            std::cout << "  Path for " << endpoint_block << ":" << endpoint_port << ":" << std::endl;
            for (const auto& edge : path) {
                try {
                    // Check if connection already exists
                    auto active_conns = graph->enumerate_active_connections();
                    bool exists = false;
                    for (const auto& active : active_conns) {
                        if (active.src_blockid == edge.src_blockid &&
                            active.src_port == edge.src_port &&
                            active.dst_blockid == edge.dst_blockid &&
                            active.dst_port == edge.dst_port) {
                            exists = true;
                            break;
                        }
                    }
                    
                    if (!exists) {
                        std::cout << "    Connecting: " << edge.src_blockid << ":" << edge.src_port
                                 << " -> " << edge.dst_blockid << ":" << edge.dst_port << std::endl;
                        graph->connect(edge.src_blockid, edge.src_port,
                                      edge.dst_blockid, edge.dst_port);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "    Failed to connect: " << e.what() << std::endl;
                }
            }
        }
    }
}

// Per-stream capture function
template <typename samp_type>
void capture_stream(StreamContext& ctx, 
                   std::atomic<bool>& start_capture,
                   size_t num_packets) {
    
    // Wait for synchronized start if needed
    while (!start_capture.load()) {
        std::this_thread::sleep_for(1ms);
    }
    
    ctx.stats.start_time = std::chrono::steady_clock::now();
    
    // Setup buffers
    std::vector<samp_type> buff(ctx.samps_per_buff);
    std::vector<void*> buff_ptrs = {&buff.front()};
    uhd::rx_metadata_t md;
    
    // Stream command
    uhd::stream_cmd_t stream_cmd(num_packets == 0 ? 
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS :
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    
    if (num_packets > 0) {
        stream_cmd.num_samps = num_packets * ctx.samps_per_buff;
    }
    
    stream_cmd.stream_now = true;
    ctx.rx_streamer->issue_stream_cmd(stream_cmd);
    
    std::cout << "[Stream " << ctx.stream_id << "] Started capture from " 
              << ctx.block_id << ":" << ctx.port << std::endl;
    
    // Main capture loop
    while (!stop_signal_called && (num_packets == 0 || ctx.stats.packets_captured < num_packets)) {
        size_t num_rx_samps = ctx.rx_streamer->recv(buff_ptrs, ctx.samps_per_buff, md, 3.0);
        
        // Handle errors
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << "[Stream " << ctx.stream_id << "] Timeout while streaming" << std::endl;
            break;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            ctx.stats.overflow_count++;
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "[Stream " << ctx.stream_id << "] Receiver error: " 
                      << md.strerror() << std::endl;
            ctx.stats.error_count++;
            break;
        }
        
        if (num_rx_samps == 0) continue;
        
        // Build CHDR packet
        std::vector<uint8_t> packet_buffer;
        
        // Calculate sizes
        size_t payload_bytes = num_rx_samps * sizeof(samp_type);
        size_t header_bytes = 8;
        size_t timestamp_bytes = md.has_time_spec ? 8 : 0;
        size_t total_chdr_bytes = header_bytes + timestamp_bytes + payload_bytes;
        
        // Build header
        uint64_t header = 0;
        header |= (uint64_t)ctx.stream_id & 0xFFFF;              // Use stream_id as EPID
        header |= ((uint64_t)total_chdr_bytes & 0xFFFF) << 16;   // Length
        header |= ((uint64_t)ctx.stats.packets_captured & 0xFFFF) << 32;   // Sequence
        header |= ((uint64_t)0 & 0x1F) << 48;                    // NumMData
        header |= ((uint64_t)(md.has_time_spec ? PKT_TYPE_DATA_WITH_TS : PKT_TYPE_DATA_NO_TS) & 0x7) << 53;
        header |= ((uint64_t)(md.end_of_burst ? 1 : 0) & 0x1) << 57; // EOB
        
        // Write header
        write_le(packet_buffer, header);
        
        // Write timestamp if present
        uint64_t timestamp_ticks = 0;
        if (md.has_time_spec) {
            timestamp_ticks = md.time_spec.to_ticks(ctx.tick_rate);
            write_le(packet_buffer, timestamp_ticks);
            
            // Track first and last timestamps
            double timestamp_sec = md.time_spec.get_real_secs();
            if (ctx.stats.first_timestamp == 0.0) {
                ctx.stats.first_timestamp = timestamp_sec;
            }
            ctx.stats.last_timestamp = timestamp_sec;
        }
        
        // Write payload
        const uint8_t* sample_bytes = reinterpret_cast<const uint8_t*>(buff.data());
        packet_buffer.insert(packet_buffer.end(), sample_bytes, sample_bytes + payload_bytes);
        
        // Write to file (with mutex for shared file)
        if (ctx.output_file) {
            if (ctx.file_mutex) {
                std::lock_guard<std::mutex> lock(*ctx.file_mutex);
                uint32_t pkt_size = static_cast<uint32_t>(packet_buffer.size());
                ctx.output_file->write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
                ctx.output_file->write(reinterpret_cast<const char*>(packet_buffer.data()), 
                                      packet_buffer.size());
            } else {
                // Separate file, no mutex needed
                uint32_t pkt_size = static_cast<uint32_t>(packet_buffer.size());
                ctx.output_file->write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
                ctx.output_file->write(reinterpret_cast<const char*>(packet_buffer.data()), 
                                      packet_buffer.size());
            }
        }
        
        // Store for analysis
        if (ctx.analysis_packets && ctx.analysis_packets->size() < 10000) {
            chdr_packet_data pkt;
            pkt.stream_id = ctx.stream_id;
            pkt.stream_block = ctx.block_id;
            pkt.stream_port = ctx.port;
            pkt.header_raw = header;
            pkt.parse_header();
            
            if (md.has_time_spec) {
                pkt.timestamp = timestamp_ticks;
            }
            pkt.payload.assign(sample_bytes, sample_bytes + payload_bytes);
            
            std::lock_guard<std::mutex> lock(*ctx.analysis_mutex);
            ctx.analysis_packets->push_back(pkt);
        }
        
        // Update statistics
        ctx.stats.packets_captured++;
        ctx.stats.total_samples += num_rx_samps;
        
        // Progress update every 1000 packets
        if (ctx.stats.packets_captured % 1000 == 0) {
            std::cout << "[Stream " << ctx.stream_id << "] Packets: " 
                      << ctx.stats.packets_captured 
                      << ", Samples: " << ctx.stats.total_samples;
            if (ctx.stats.overflow_count > 0) {
                std::cout << " (O: " << ctx.stats.overflow_count << ")";
            }
            std::cout << std::endl;
        }
    }
    
    // Stop streaming
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    ctx.rx_streamer->issue_stream_cmd(stream_cmd);
    
    ctx.stats.end_time = std::chrono::steady_clock::now();
    
    // Final statistics for this stream
    double elapsed_secs = std::chrono::duration<double>(ctx.stats.end_time - ctx.stats.start_time).count();
    std::cout << "\n[Stream " << ctx.stream_id << "] Capture complete:" << std::endl;
    std::cout << "  Block: " << ctx.block_id << ":" << ctx.port << std::endl;
    std::cout << "  Packets: " << ctx.stats.packets_captured << std::endl;
    std::cout << "  Samples: " << ctx.stats.total_samples << std::endl;
    std::cout << "  Duration: " << elapsed_secs << " seconds" << std::endl;
    std::cout << "  Average rate: " << (ctx.stats.total_samples/elapsed_secs)/1e6 << " Msps" << std::endl;
    if (ctx.stats.overflow_count > 0) {
        std::cout << "  Overflows: " << ctx.stats.overflow_count << std::endl;
    }
}

// Main multi-stream capture function
template <typename samp_type>
void capture_multi_stream(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const GraphConfig& config,
    const std::string& file,
    size_t num_packets,
    bool enable_analysis,
    const std::string& csv_file,
    double rate,
    size_t samps_per_buff)
{
    // Print initial graph state
    print_graph_info(graph);
    
    // 1. First, just initialize blocks without setting properties
    if (!config.block_init_order.empty()) {
        std::cout << "\nInitializing blocks in specified order..." << std::endl;
        for (const auto& block_id : config.block_init_order) {
            try {
                auto block = graph->get_block(uhd::rfnoc::block_id_t(block_id));
                if (block) {
                    std::cout << "  Initialized: " << block_id << std::endl;
                }
            } catch (...) {
                std::cerr << "  Warning: Could not initialize " << block_id << std::endl;
            }
        }
    }
    
    // 2. Configure switchboards
    if (!config.switchboard_configs.empty()) {
        std::cout << "\nConfiguring switchboards..." << std::endl;
        configure_switchboards(graph, config.switchboard_configs);
    }
    
    // 3. Apply dynamic connections
    if (!config.dynamic_connections.empty()) {
        std::cout << "\nApplying dynamic connections..." << std::endl;
        apply_dynamic_connections(graph, config.dynamic_connections, config.commit_after_each_connection);
    }
    
    // 4. Connect Radio to DDC if needed (but DON'T set rates yet)
    if (config.auto_connect_radio_to_ddc) {
        std::cout << "\nAuto-connecting Radio to DDC..." << std::endl;
        auto_connect_radio_to_ddc(graph);
    }
    
    // 5. Find streaming endpoints
    auto endpoints = find_all_stream_endpoints_comprehensive(graph, config.stream_endpoints, config.multi_stream);
    
    if (endpoints.empty()) {
        throw std::runtime_error("No suitable streaming endpoints found");
    }
    
    std::cout << "\nFound " << endpoints.size() << " streaming endpoints:" << std::endl;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        std::cout << "  Stream " << i << ": " << endpoints[i].first 
                  << ":" << endpoints[i].second << std::endl;
    }

    
    
    // 6. Get tick rate from Radio
    double tick_rate = DEFAULT_TICKRATE;
    auto radio_blocks = graph->find_blocks("Radio");
    if (!radio_blocks.empty()) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_blocks[0]);
        tick_rate = radio->get_tick_rate();
    }
    
    // 7. Create output files
    std::vector<StreamContext> contexts;
    std::vector<std::unique_ptr<std::ofstream>> output_files;
    std::unique_ptr<std::mutex> shared_file_mutex;
    std::vector<chdr_packet_data> all_analysis_packets;
    std::mutex analysis_mutex;
    
// std::vector<std::unique_ptr<std::ofstream>> output_files;
// std::unique_ptr<std::mutex> shared_file_mutex;   // keep this

if (enable_analysis) {
    // Decide whether we need one shared CSV or one per stream
    const bool one_file_per_stream = config.multi_stream.separate_files;

    auto make_name = [&](size_t idx) {
        if (!one_file_per_stream)          // single shared file
            return csv_file;
        // insert _<idx> before the extension (or append .csv)
        auto dot = csv_file.find_last_of('.');
        return dot != std::string::npos
                 ? csv_file.substr(0, dot) + "_" + std::to_string(idx) +
                   csv_file.substr(dot)
                 : csv_file + "_" + std::to_string(idx) + ".csv";
    };

    for (size_t i = 0; i < (one_file_per_stream ? endpoints.size() : 1); ++i) {
        auto fname = make_name(i);
        output_files.emplace_back(std::make_unique<std::ofstream>(fname));
        if (!output_files.back()->is_open()) {
            throw std::runtime_error("Failed to open analysis file: " + fname);
        }
    }

    if (!one_file_per_stream) {
        // different threads will write to the same file
        shared_file_mutex = std::make_unique<std::mutex>();
    }
}
    
    // 8. Create streamers and connect them (following rfnoc_rx_to_file pattern)
    std::vector<uhd::rfnoc::ddc_block_control::sptr> ddc_controls;
    std::vector<size_t> ddc_channels;

    // establish_signal_paths(graph, config, endpoints);
    std::cout << "Making manual bespoke connections, lets see" <<std::endl;
    try {
        // Connect Radio to DDC explicitly through the chain
        // This tells the graph about the data path
        graph->connect(uhd::rfnoc::block_id_t("0/Radio#0"), 0, uhd::rfnoc::block_id_t("0/Switchboard#0"), 0, false);
        graph->connect(uhd::rfnoc::block_id_t("0/Switchboard#0"), 1, uhd::rfnoc::block_id_t("0/SplitStream#1"), 0, false);
        graph->connect(uhd::rfnoc::block_id_t("0/SplitStream#1"), 0, uhd::rfnoc::block_id_t("0/Switchboard#1"), 2, false);
        graph->connect(uhd::rfnoc::block_id_t("0/SplitStream#1"), 1, uhd::rfnoc::block_id_t("0/Switchboard#1"), 3, false);
        graph->connect(uhd::rfnoc::block_id_t("0/SplitStream#1"), 2, uhd::rfnoc::block_id_t("0/Switchboard#1"), 4, false);
        graph->connect(uhd::rfnoc::block_id_t("0/SplitStream#1"), 3, uhd::rfnoc::block_id_t("0/Switchboard#1"), 5, false);
        graph->connect(uhd::rfnoc::block_id_t("0/Switchboard#1"), 0, uhd::rfnoc::block_id_t("0/DDC#0"), 0, false); // true = is_back_edge
        graph->connect(uhd::rfnoc::block_id_t("0/Switchboard#1"), 1, uhd::rfnoc::block_id_t("0/DDC#0"), 1, false);
        graph->connect(uhd::rfnoc::block_id_t("0/Switchboard#1"), 2, uhd::rfnoc::block_id_t("0/DDC#0"), 2, false);
        graph->connect(uhd::rfnoc::block_id_t("0/Switchboard#1"), 3, uhd::rfnoc::block_id_t("0/DDC#0"), 3, false);
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect ddcs to radio: " << e.what() << std::endl;
    }
    
    for (size_t i = 0; i < endpoints.size(); ++i) {
        const auto& [block_id, port] = endpoints[i];
        
        std::cout << "Setting up stream " << i << " for " << block_id << ":" << port << std::endl;
        
        try {
            uhd::rfnoc::block_id_t endpoint_id(block_id);
            
            // Check if this endpoint actually exists with the specified port
            auto endpoint_block = graph->get_block(endpoint_id);
            if (!endpoint_block || port >= endpoint_block->get_num_output_ports()) {
                std::cerr << "Warning: " << block_id << " port " << port 
                        << " doesn't exist in current FPGA image" << std::endl;
                continue;
            }
            
            // Detect if we have switchboards in this image
            bool has_switchboards = !graph->find_blocks("Switchboard").empty();
            
            // For DDC endpoints, handle based on architecture
            if (endpoint_id.get_block_name() == "DDC") {
                // Check if there's a direct Radio→DDC connection in static connections
                bool has_direct_radio_connection = false;
                auto static_conns = graph->enumerate_static_connections();
                
                for (const auto& conn : static_conns) {
                    if (conn.dst_blockid == block_id && conn.dst_port == port) {
                        // Check if source is a Radio
                        uhd::rfnoc::block_id_t src_id(conn.src_blockid);
                        if (src_id.get_block_name() == "Radio") {
                            has_direct_radio_connection = true;
                            break;
                        }
                    }
                }
                
                // If we have direct Radio→DDC connections and no switchboards, 
                // we need to establish the connection path
                if (has_direct_radio_connection && !has_switchboards) {
                    // Find which radio connects to this DDC port
                    std::string radio_block_id;
                    size_t radio_port = 0;
                    
                    for (const auto& conn : static_conns) {
                        if (conn.dst_blockid == block_id && conn.dst_port == port) {
                            uhd::rfnoc::block_id_t src_id(conn.src_blockid);
                            if (src_id.get_block_name() == "Radio") {
                                radio_block_id = conn.src_blockid;
                                radio_port = conn.src_port;
                                break;
                            }
                        }
                    }
                    
                    if (!radio_block_id.empty()) {
                        // Connect through the static path
                        std::cout << "  Connecting through Radio path: " << radio_block_id 
                                << ":" << radio_port << " -> " << block_id << ":" << port << std::endl;
                        
                        // Use connect_through_blocks for direct connections
                        uhd::rfnoc::connect_through_blocks(
                            graph,
                            radio_block_id, radio_port,
                            block_id, port);
                    }
                }
                // For switchboard-based architectures, the routing is already configured
                else if (has_switchboards) {
                    std::cout << "  Using pre-configured switchboard routing for " 
                            << block_id << ":" << port << std::endl;
                }
            }
            
            // Create stream args
            uhd::stream_args_t stream_args("sc16", "sc16");
            stream_args.channels = {0};
            stream_args.args = uhd::device_addr_t();
            
            // Apply stream args from config
            for (const auto& sep : config.stream_endpoints) {
                if (sep.block_id == block_id && sep.port == port) {
                    for (const auto& [key, value] : sep.stream_args) {
                        stream_args.args[key] = value;
                    }
                    break;
                }
            }
            
            // Create RX streamer
            auto rx_streamer = graph->create_rx_streamer(1, stream_args);
            
            // Connect streamer to endpoint
            graph->connect(block_id, port, rx_streamer, 0, true);
            
            std::cout << "  Connected RX streamer to " << block_id << ":" << port << std::endl;
            
            // Store DDC control for later rate setting
            if (endpoint_id.get_block_name() == "DDC") {
                auto ddc_ctrl = graph->get_block<uhd::rfnoc::ddc_block_control>(endpoint_id);
                if (ddc_ctrl) {
                    ddc_controls.push_back(ddc_ctrl);
                    ddc_channels.push_back(port);
                }
            }
            

            
            

            std::cout << "  Connected RX streamer" << std::endl;
            
            // Create context
            StreamContext ctx;
            ctx.stream_id = i;
            ctx.block_id = block_id;
            ctx.port = port;
            ctx.rx_streamer = rx_streamer;
            ctx.output_file = config.multi_stream.separate_files ? 
                             output_files[i].get() : output_files[0].get();
            ctx.file_mutex = config.multi_stream.separate_files ? 
                            nullptr : shared_file_mutex.get();
            ctx.stats.stream_id = i;
            ctx.stats.block_id = block_id;
            ctx.stats.port = port;
            ctx.analysis_packets = enable_analysis ? &all_analysis_packets : nullptr;
            ctx.analysis_mutex = &analysis_mutex;
            ctx.tick_rate = tick_rate;
            ctx.samps_per_buff = samps_per_buff;
            ctx.separate_file = config.multi_stream.separate_files;
            
            contexts.push_back(ctx);
            
        } catch (const std::exception& e) {
            std::cerr << "Failed to setup stream " << i << ": " << e.what() << std::endl;
        }
    }

    // 9. COMMIT THE GRAPH BEFORE SETTING PROPERTIES
    std::cout << "\nCommitting graph..." << std::endl;
    graph->commit();
    
    // 10. NOW set block properties (especially sample rates) AFTER commit
    std::cout << "\nSetting block properties after commit..." << std::endl;
    
    // Set Radio properties first
    if (!radio_blocks.empty()) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_blocks[0]);
        if (radio && config.block_properties.find("0/Radio#0") != config.block_properties.end()) {
            auto& props = config.block_properties.at("0/Radio#0");
            
            // Set radio rate
            if (props.find("rate") != props.end()) {
                radio->set_rate(std::stod(props.at("rate")));
            }
            
            // Set other radio properties
            for (size_t chan = 0; chan < radio->get_num_output_ports(); ++chan) {
                if (props.find("freq/" + std::to_string(chan)) != props.end()) {
                    radio->set_rx_frequency(std::stod(props.at("freq/" + std::to_string(chan))), chan);
                }
                if (props.find("gain/" + std::to_string(chan)) != props.end()) {
                    radio->set_rx_gain(std::stod(props.at("gain/" + std::to_string(chan))), chan);
                }
                if (props.find("antenna/" + std::to_string(chan)) != props.end()) {
                    radio->set_rx_antenna(props.at("antenna/" + std::to_string(chan)), chan);
                }
            }
        }
    }
    
    // Set DDC output rates AFTER commit (like rfnoc_rx_to_file does)
    for (size_t i = 0; i < ddc_controls.size(); ++i) {
        auto ddc = ddc_controls[i];
        size_t chan = ddc_channels[i];
        
        std::cout << "Setting DDC output rate for channel " << chan << " to " << rate << " Hz" << std::endl;
        double actual_rate = ddc->set_output_rate(rate, chan);
        std::cout << "  Actual rate: " << actual_rate << " Hz" << std::endl;
    }
    
    // Print final graph state
    std::cout << "\n=== Final Graph State ===" << std::endl;
    print_graph_info(graph);
    
    std::cout << "\nStream configuration:" << std::endl;
    std::cout << "  Number of streams: " << contexts.size() << std::endl;
    std::cout << "  Sample rate: " << rate/1e6 << " Msps" << std::endl;
    std::cout << "  Tick rate: " << tick_rate/1e6 << " MHz" << std::endl;
    std::cout << "  Samples per buffer: " << samps_per_buff << std::endl;
    std::cout << "  Stream synchronization: " << (config.multi_stream.sync_streams ? "Yes" : "No") << std::endl;
    std::cout << "  Output mode: " << (config.multi_stream.separate_files ? "Separate files" : "Single file") << std::endl;
    
    // Create capture threads
    std::vector<std::thread> capture_threads;
    std::atomic<bool> start_capture(false);
    
    // Start capture threads
    for (auto& ctx : contexts) {
        capture_threads.emplace_back(capture_stream<samp_type>, 
                                    std::ref(ctx), 
                                    std::ref(start_capture), 
                                    num_packets);
    }
    
    // Synchronize start if requested
    if (config.multi_stream.sync_streams) {
        std::cout << "\nSynchronizing streams..." << std::endl;
        std::this_thread::sleep_for(std::chrono::duration<double>(config.multi_stream.sync_delay));
    }
    
    // Start all captures
    auto overall_start = std::chrono::steady_clock::now();
    start_capture.store(true);
    std::cout << "\nStarting multi-stream capture..." << std::endl;
    
    // Wait for all threads to complete
    for (auto& thread : capture_threads) {
        thread.join();
    }
    
    auto overall_end = std::chrono::steady_clock::now();
    double overall_duration = std::chrono::duration<double>(overall_end - overall_start).count();
    
    // Collect statistics
    std::vector<StreamStats> all_stats;
    size_t total_packets = 0;
    size_t total_samples = 0;
    size_t total_overflows = 0;
    
    for (const auto& ctx : contexts) {
        all_stats.push_back(ctx.stats);
        total_packets += ctx.stats.packets_captured;
        total_samples += ctx.stats.total_samples;
        total_overflows += ctx.stats.overflow_count;
    }
    
    // Close files
    for (auto& file : output_files) {
        if (file && file->is_open()) {
            file->close();
        }
    }
    
    // Print overall statistics
    std::cout << "\n=== Multi-Stream Capture Statistics ===" << std::endl;
    std::cout << "Total streams: " << contexts.size() << std::endl;
    std::cout << "Total packets captured: " << total_packets << std::endl;
    std::cout << "Total samples: " << total_samples << std::endl;
    std::cout << "Overall duration: " << overall_duration << " seconds" << std::endl;
    std::cout << "Aggregate sample rate: " << (total_samples/overall_duration)/1e6 << " Msps" << std::endl;
    if (total_overflows > 0) {
        std::cout << "Total overflows: " << total_overflows << std::endl;
    }
    
    // Perform analysis
    if (enable_analysis && !csv_file.empty() && !all_analysis_packets.empty()) {
        std::cout << "\nAnalyzing packets..." << std::endl;
        
        // Sort packets by stream ID and sequence number
        std::sort(all_analysis_packets.begin(), all_analysis_packets.end(),
                 [](const chdr_packet_data& a, const chdr_packet_data& b) {
                     if (a.stream_id != b.stream_id) return a.stream_id < b.stream_id;
                     return a.seq_num < b.seq_num;
                 });
        
        analyze_packets(all_analysis_packets, csv_file, tick_rate, all_stats);
    }
}

// Main function
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // Variables
    std::string args, file, format, csv_file, yaml_config;
    size_t num_packets, spb;
    double rate, freq, gain, bw;
    bool analyze_only = false;
    bool show_graph = false;
    bool create_yaml_template = false;
    bool multi_stream = false;
    
    // Setup program options
    po::options_description desc("Allowed options");
    
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "UHD device arguments")
        ("file", po::value<std::string>(&file)->default_value("chdr_capture.dat"), "output filename")
        ("yaml", po::value<std::string>(&yaml_config)->default_value(""), "YAML configuration file")
        ("csv", po::value<std::string>(&csv_file)->default_value(""), "CSV output file for analysis")
        ("num-packets", po::value<size_t>(&num_packets)->default_value(1000), "packets per stream (0 for continuous)")
        ("rate", po::value<double>(&rate)->default_value(10e6), "sample rate")
        ("freq", po::value<double>(&freq)->default_value(100e6), "center frequency")
        ("gain", po::value<double>(&gain)->default_value(30.0), "gain")
        ("bw", po::value<double>(&bw)->default_value(0.0), "analog bandwidth")
        ("format", po::value<std::string>(&format)->default_value("sc16"), "sample format")
        ("spb", po::value<size_t>(&spb)->default_value(2000), "samples per buffer")
        ("show-graph", po::value<bool>(&show_graph)->default_value(false), "print RFNoC graph info and exit")
        ("analyze-only", po::value<bool>(&analyze_only)->default_value(false), "only analyze existing file")
        ("create-yaml-template", po::value<bool>(&create_yaml_template)->default_value(false), "create YAML template")
        ("multi-stream", po::value<bool>(&multi_stream)->default_value(false), "enable multi-stream capture")
        ("dev", po::value<bool>(&multi_stream)->default_value(true), "enable dev mode, where 1st block is assumed to be Radio0")
    ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    // Help message
    if (vm.count("help")) {
        std::cout << "CHDR Packet Capture Tool with Multi-Stream Support" << std::endl;
        std::cout << desc << std::endl;
        std::cout << "\nMulti-Stream Features:" << std::endl;
        std::cout << "  * Capture from multiple DDC blocks simultaneously" << std::endl;
        std::cout << "  * Synchronized stream start for time-aligned captures" << std::endl;
        std::cout << "  * Per-stream statistics and monitoring" << std::endl;
        std::cout << "  * Separate or combined output files" << std::endl;
        std::cout << "  * Multi-threaded capture with one thread per stream" << std::endl;
        std::cout << "\nSupported RFNoC blocks:" << std::endl;
        std::cout << "  * Radio - RF frontend control" << std::endl;
        std::cout << "  * DDC/DUC - Digital down/up converters (multi-channel)" << std::endl;
        std::cout << "  * FIR - FIR filter" << std::endl;
        std::cout << "  * FFT - Fast Fourier Transform" << std::endl;
        std::cout << "  * Window - Windowing function" << std::endl;
        std::cout << "  * ... (and many more)" << std::endl;
        std::cout << "\nNotes:" << std::endl;
        std::cout << "  * Use --multi-stream or configure multi_stream in YAML for multi-stream capture" << std::endl;
        std::cout << "  * Use --show-graph to see available blocks and connections" << std::endl;
        std::cout << "  * Use --create-yaml-template to generate a multi-stream YAML template" << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Create YAML template if requested
    if (create_yaml_template) {
        // Create graph to discover hardware
        std::cout << "Creating RFNoC graph to discover hardware..." << std::endl;
        auto graph = uhd::rfnoc::rfnoc_graph::make(args);
        
        // Generate dynamic template based on discovered hardware
        write_dynamic_yaml_template(graph, "rfnoc_config_discovered.yaml");
        return EXIT_SUCCESS;
    }
    // Signal handler
    std::signal(SIGINT, &sig_int_handler);
    
    // Create graph
    std::cout << "Creating RFNoC graph..." << std::endl;
    auto graph = uhd::rfnoc::rfnoc_graph::make(args);
    
    if (show_graph) {
        print_graph_info(graph);
        return EXIT_SUCCESS;
    }
    
    // Load configuration
    GraphConfig config;
    if (!yaml_config.empty()) {
        std::cout << "Loading configuration from: " << yaml_config << std::endl;
        config = load_graph_config(yaml_config);
    } else {
        // Set default configuration
        config.auto_connect_radio_to_ddc = true;
        config.auto_find_stream_endpoint = true;
        config.multi_stream.enable_multi_stream = multi_stream;
        
        // Add default Radio properties if Radio block exists
        auto radio_blocks = graph->find_blocks("Radio");
        if (!radio_blocks.empty()) {
            for (const auto& radio_id : radio_blocks) {
                std::string id_str = radio_id.to_string();
                config.block_properties[id_str]["freq"] = std::to_string(freq);
                config.block_properties[id_str]["gain"] = std::to_string(gain);
                config.block_properties[id_str]["rate"] = std::to_string(rate);
                if (bw > 0) {
                    config.block_properties[id_str]["bandwidth"] = std::to_string(bw);
                }
            }
        }
    }
    
    // Start capture
    bool enable_analysis = !csv_file.empty();
    
    // Use multi-stream capture if enabled
    if (config.multi_stream.enable_multi_stream) {
        if (format == "sc16") {
            capture_multi_stream<std::complex<short>>(
                graph, config, file, num_packets, enable_analysis, csv_file, rate, spb);
        } else if (format == "fc32") {
            capture_multi_stream<std::complex<float>>(
                graph, config, file, num_packets, enable_analysis, csv_file, rate, spb);
        } else {
            throw std::runtime_error("Unsupported format: " + format);
        }
    } else {
        // Single-stream capture (original functionality)
        std::cerr << "Single-stream capture not implemented in this version. "
                  << "Please use --multi-stream or configure multi_stream in YAML." << std::endl;
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}