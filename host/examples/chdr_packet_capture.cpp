//
// Copyright 2024 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CHDR Packet Capture Tool - Enhanced with Full RFNoC Block Support
//
// This utility captures sample data via UHD streaming API with support for:
// - All RFNoC block types (Radio, DDC, DUC, FFT, FIR, etc.)
// - Dynamic and static block chain discovery
// - Comprehensive YAML-based configuration
// - Automatic and manual SEP (Stream Endpoint) handling
// - Advanced switchboard routing for complex graphs
//

#include <uhd/exception.hpp>
#include <uhd/rfnoc_graph.hpp>
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

#define DEFAULT_TICKRATE 200000000

namespace po = boost::program_options;
using namespace std::chrono_literals;

// Global flag for Ctrl+C handling
static volatile bool stop_signal_called = false;

void sig_int_handler(int)
{
    stop_signal_called = true;
}

/***********************************************************************
 *  Automatically connect every Radio output to the matching DDC input
 *  ------------------------------------------------------------------
 *  – Searches all Radio* and DDC* blocks that exist in the graph.
 *  – Matches blocks by *device number* and *block counter*  
 *    (e.g. “0/Radio#0” ↔ “0/DDC#0”).  
 *  – Optionally wires any un-connected *linear* DSP blocks that
 *    belong to the same channel (FIR, Window, FFT, Switchboard, …).
 *  – Skips a connection if `graph->is_connectable()` returns false or
 *    if the edge already exists.
 *  – Returns **true** if at least one Radio↔DDC path was created.
 **********************************************************************/
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

    const std::vector<std::string> optional_chain = {
        "Switchboard", "FIR", "Window", "FFT",
        "MovingAverage", "VectorIIR", "KeepOneInN"
    };

    for (const auto& radio_id : radio_blocks) {
        const size_t dev  = radio_id.get_device_no();   // e.g. 0
        const size_t chan = radio_id.get_block_count(); // e.g. 0,1 … :contentReference[oaicite:0]{index=0}


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

        block_id_t prev_blk = radio_id;
        size_t     prev_p   = 0; // always use port 0 for standard blocks

        // ---- scan optional single-port DSP blocks ---------------------
        for (const auto& blk_name : optional_chain) {
            block_id_t candidate(dev, blk_name, chan);
            if (!graph->has_block(candidate)) {
                continue;                // block not present in this image
            }
            if (edge_exists(prev_blk, prev_p, candidate, 0)) {
                prev_blk = candidate;    // already wired – hop forward
                continue;
            }
            if (!graph->is_connectable(prev_blk, prev_p, candidate, 0)) {
                break; // topology disallows further chaining
            }
            graph->connect(prev_blk, prev_p, candidate, 0);          // :contentReference[oaicite:1]{index=1}
            prev_blk = candidate;
            made_connection = true;
        }

        if (!edge_exists(prev_blk, prev_p, ddc_match, 0) &&
            graph->is_connectable(prev_blk, prev_p, ddc_match, 0))
        {
            graph->connect(prev_blk, prev_p, ddc_match, 0);          // :contentReference[oaicite:2]{index=2}
            made_connection = true;
        }
    }

    if (!made_connection) {
        std::cerr << "[auto-connect] No new Radio→DDC paths were created "
                  << "(everything was already wired or not connectable)\n";
    }
    return made_connection;
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

// Structure to hold parsed CHDR packet data
struct chdr_packet_data {
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

// Stream endpoint configuration
struct StreamEndpointConfig {
    std::string block_id;
    size_t port;
    std::string direction; // "rx" or "tx"
    std::map<std::string, std::string> stream_args;
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

// Graph configuration from YAML
struct GraphConfig {
    std::vector<ConnectionConfig> dynamic_connections;
    std::vector<SwitchboardConfig> switchboard_configs;
    std::map<std::string, std::map<std::string, std::string>> block_properties;
    std::vector<StreamEndpointConfig> stream_endpoints;
    bool auto_connect_radio_to_ddc = true;
    bool auto_find_stream_endpoint = true;
    bool commit_after_each_connection = false;
    
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

// Forward declarations
bool auto_connect_radio_to_ddc(uhd::rfnoc::rfnoc_graph::sptr graph);

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

// Enhanced YAML configuration loader
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
        
        // Load stream endpoint configurations
        if (root["stream_endpoints"]) {
            for (const auto& sep : root["stream_endpoints"]) {
                StreamEndpointConfig sec;
                sec.block_id = sep["block_id"].as<std::string>();
                sec.port = sep["port"].as<size_t>(0);
                sec.direction = sep["direction"].as<std::string>("rx");
                if (sep["stream_args"]) {
                    for (const auto& arg : sep["stream_args"]) {
                        sec.stream_args[arg.first.as<std::string>()] = arg.second.as<std::string>();
                    }
                }
                config.stream_endpoints.push_back(sec);
            }
        }
        
        // Load block properties - enhanced for all block types
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
            config.auto_connect_radio_to_ddc = root["auto_connect"]["radio_to_ddc"].as<bool>(true);
            config.auto_find_stream_endpoint = root["auto_connect"]["find_stream_endpoint"].as<bool>(true);
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

// Enhanced block discovery with property detection
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
                id.to_string().find("SEP") != std::string::npos) {
                info.has_stream_endpoint = true;
            }
            
            // Try to detect if block has SEP by checking for streaming capability
            try {
                // This is a heuristic - blocks that can stream typically have certain properties
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
                // Note: Property types cannot be determined without template parameter
            } catch (...) {}
        }
        
        blocks.push_back(info);
    }
    
    return blocks;
}

// Apply block properties for all RFNoC block types
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
            // Handle DUC block properties
            else if (id.get_block_name() == "DUC") {
                auto duc = std::dynamic_pointer_cast<uhd::rfnoc::duc_block_control>(block);
                if (duc) {
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
                            duc->set_freq(std::stod(value), chan);
                            std::cout << "  Set DUC frequency[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "input_rate") {
                            duc->set_input_rate(std::stod(value), chan);
                            std::cout << "  Set input rate[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "output_rate") {
                            duc->set_output_rate(std::stod(value), chan);
                            std::cout << "  Set output rate[" << chan << "]: " << value << std::endl;
                        }
                    }
                }
            }
            // Handle FIR Filter block properties
            else if (id.get_block_name() == "FIR") {
                auto fir = std::dynamic_pointer_cast<uhd::rfnoc::fir_filter_block_control>(block);
                if (fir) {
                    for (const auto& [prop, value] : props) {
                        size_t chan = 0;
                        std::regex chan_regex("(.+)/(\\d+)");
                        std::smatch match;
                        std::string prop_name = prop;
                        if (std::regex_match(prop, match, chan_regex)) {
                            prop_name = match[1];
                            chan = std::stoul(match[2]);
                        }
                        
                        if (prop_name == "coeffs") {
                            // Parse comma-separated coefficients
                            std::vector<int16_t> coeffs;
                            std::stringstream ss(value);
                            std::string coeff;
                            while (std::getline(ss, coeff, ',')) {
                                coeffs.push_back(std::stoi(coeff));
                            }
                            fir->set_coefficients(coeffs, chan);
                            std::cout << "  Set FIR coefficients[" << chan << "]: " << coeffs.size() << " taps" << std::endl;
                        }
                    }
                }
            }
            // Handle FFT block properties
            else if (id.get_block_name() == "FFT") {
                auto fft = std::dynamic_pointer_cast<uhd::rfnoc::fft_block_control>(block);
                if (fft) {
                    for (const auto& [prop, value] : props) {
                        if (prop == "length") {
                            fft->set_length(std::stoul(value));
                            std::cout << "  Set FFT length: " << value << std::endl;
                        } else if (prop == "magnitude" || prop == "magnitude_out") {
                            fft->set_magnitude(value == "true" || value == "1" ? 
                                uhd::rfnoc::fft_magnitude::MAGNITUDE :
                                uhd::rfnoc::fft_magnitude::COMPLEX);
                            std::cout << "  Set magnitude output: " << value << std::endl;
                        } else if (prop == "direction") {
                            fft->set_direction(value == "reverse" ? 
                                uhd::rfnoc::fft_direction::REVERSE :
                                uhd::rfnoc::fft_direction::FORWARD);
                            std::cout << "  Set FFT direction: " << value << std::endl;
                        } else if (prop == "scaling") {
                            fft->set_scaling(std::stoul(value));
                            std::cout << "  Set FFT scaling: " << value << std::endl;
                        } else if (prop == "shift") {
                            fft->set_shift_config(value == "normal" ? 
                                uhd::rfnoc::fft_shift::NORMAL :
                                value == "reverse" ?
                                uhd::rfnoc::fft_shift::REVERSE :
                                uhd::rfnoc::fft_shift::NATURAL);
                            std::cout << "  Set FFT shift: " << value << std::endl;
                        }
                    }
                }
            }
            // Handle Window block properties
            else if (id.get_block_name() == "Window") {
                auto window = std::dynamic_pointer_cast<uhd::rfnoc::window_block_control>(block);
                if (window) {
                    for (const auto& [prop, value] : props) {
                        size_t chan = 0;
                        std::regex chan_regex("(.+)/(\\d+)");
                        std::smatch match;
                        std::string prop_name = prop;
                        if (std::regex_match(prop, match, chan_regex)) {
                            prop_name = match[1];
                            chan = std::stoul(match[2]);
                        }
                        
                        if (prop_name == "coeffs") {
                            // Parse comma-separated coefficients
                            std::vector<int16_t> coeffs;
                            std::stringstream ss(value);
                            std::string coeff;
                            while (std::getline(ss, coeff, ',')) {
                                coeffs.push_back(std::stoi(coeff));
                            }
                            window->set_coefficients(coeffs, chan);
                            std::cout << "  Set window coefficients[" << chan << "]: " << coeffs.size() << " values" << std::endl;
                        }
                    }
                }
            }
            // Handle Signal Generator block properties
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
                            siggen->set_enable(value == "true" || value == "1", chan);
                            std::cout << "  Set signal generator enable[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "waveform") {
                            if (value == "sine") {
                                siggen->set_waveform(uhd::rfnoc::siggen_waveform::SINE_WAVE, chan);
                            } else if (value == "noise") {
                                siggen->set_waveform(uhd::rfnoc::siggen_waveform::NOISE, chan);
                            } else if (value == "constant") {
                                siggen->set_waveform(uhd::rfnoc::siggen_waveform::CONSTANT, chan);
                            }
                            std::cout << "  Set waveform[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "amplitude") {
                            siggen->set_amplitude(std::stod(value), chan);
                            std::cout << "  Set amplitude[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "frequency") {
                            // For sine frequency, need sample rate
                            // The frequency parameter is in Hz, and requires sample rate
                            double sample_rate = default_rate; // Use the default rate
                            try {
                                // Try to get actual sample rate from connected blocks
                                auto edges = graph->enumerate_active_connections();
                                for (const auto& edge : edges) {
                                    if (edge.dst_blockid == block_id) {
                                        // Found upstream connection
                                        if (edge.src_blockid.find("Radio") != std::string::npos) {
                                            auto radio = graph->get_block<uhd::rfnoc::radio_control>(edge.src_blockid);
                                            sample_rate = radio->get_rate();
                                        } else if (edge.src_blockid.find("DDC") != std::string::npos) {
                                            auto ddc = graph->get_block<uhd::rfnoc::ddc_block_control>(edge.src_blockid);
                                            sample_rate = ddc->get_output_rate(edge.src_port);
                                        }
                                        break;
                                    }
                                }
                            } catch (...) {}
                            
                            siggen->set_sine_frequency(std::stod(value), sample_rate, chan);
                            std::cout << "  Set sine frequency[" << chan << "]: " << value << " Hz (sample rate: " << sample_rate << ")" << std::endl;
                        } else if (prop_name == "constant_i" || prop_name == "constant_q") {
                            // For constant waveform
                            auto i_val = siggen->get_constant(chan).real();
                            auto q_val = siggen->get_constant(chan).imag();
                            if (prop_name == "constant_i") i_val = std::stod(value);
                            else q_val = std::stod(value);
                            siggen->set_constant(std::complex<double>(i_val, q_val), chan);
                            std::cout << "  Set constant[" << chan << "] " << prop_name << ": " << value << std::endl;
                        }
                    }
                }
            }
            // Handle Replay block properties
            else if (id.get_block_name() == "Replay") {
                auto replay = std::dynamic_pointer_cast<uhd::rfnoc::replay_block_control>(block);
                if (replay) {
                    // Note: Replay block configuration is typically done through API calls
                    // during runtime, not through properties
                    std::cout << "  Replay block configuration:" << std::endl;
                    for (const auto& [prop, value] : props) {
                        std::cout << "    " << prop << " = " << value << " (configure via API)" << std::endl;
                    }
                }
            }
            // Handle Moving Average block properties
            else if (id.get_block_name() == "MovingAverage") {
                auto mavg = std::dynamic_pointer_cast<uhd::rfnoc::moving_average_block_control>(block);
                if (mavg) {
                    for (const auto& [prop, value] : props) {
                        if (prop == "sum_len") {
                            mavg->set_sum_len(static_cast<uint8_t>(std::stoul(value)));
                            std::cout << "  Set sum length: " << value << std::endl;
                        } else if (prop == "divisor") {
                            mavg->set_divisor(static_cast<uint32_t>(std::stoul(value)));
                            std::cout << "  Set divisor: " << value << std::endl;
                        }
                    }
                }
            }
            // Handle Vector IIR block properties
            else if (id.get_block_name() == "VectorIIR") {
                auto viir = std::dynamic_pointer_cast<uhd::rfnoc::vector_iir_block_control>(block);
                if (viir) {
                    for (const auto& [prop, value] : props) {
                        size_t chan = 0;
                        std::regex chan_regex("(.+)/(\\d+)");
                        std::smatch match;
                        std::string prop_name = prop;
                        if (std::regex_match(prop, match, chan_regex)) {
                            prop_name = match[1];
                            chan = std::stoul(match[2]);
                        }
                        
                        if (prop_name == "alpha") {
                            viir->set_alpha(std::stod(value), chan);
                            std::cout << "  Set alpha[" << chan << "]: " << value << std::endl;
                        } else if (prop_name == "beta") {
                            viir->set_beta(std::stod(value), chan);
                            std::cout << "  Set beta[" << chan << "]: " << value << std::endl;
                        }
                    }
                }
            }
            // Handle Keep One in N block properties
            else if (id.get_block_name() == "KeepOneInN") {
                auto k1n = std::dynamic_pointer_cast<uhd::rfnoc::keep_one_in_n_block_control>(block);
                if (k1n) {
                    for (const auto& [prop, value] : props) {
                        size_t chan = 0;
                        std::regex chan_regex("(.+)/(\\d+)");
                        std::smatch match;
                        std::string prop_name = prop;
                        if (std::regex_match(prop, match, chan_regex)) {
                            prop_name = match[1];
                            chan = std::stoul(match[2]);
                        }
                        
                        if (prop_name == "n") {
                            k1n->set_n(std::stoul(value), chan);
                            std::cout << "  Set N[" << chan << "]: " << value << std::endl;
                        }
                    }
                }
            }
            // Handle Add/Sub block properties
            else if (id.get_block_name() == "AddSub") {
                auto addsub = std::dynamic_pointer_cast<uhd::rfnoc::addsub_block_control>(block);
                if (addsub) {
                    // Add/Sub typically has no runtime configurable properties
                    std::cout << "  Add/Sub block has no configurable properties" << std::endl;
                }
            }
            // Handle generic properties for any block type
            else {
                std::cout << "  Block type " << id.get_block_name() << " - attempting generic property setting" << std::endl;
                for (const auto& [prop, value] : props) {
                    try {
                        // Try to set as a generic property
                        // This would require knowing the property type, which isn't easily available
                        std::cout << "  Would set property " << prop << " = " << value 
                                  << " (generic property setting not implemented)" << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "  Failed to set property " << prop << ": " << e.what() << std::endl;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to set properties for " << block_id << ": " << e.what() << std::endl;
            return false;
        }
    }
    return true;
}

// Discover block chain with enhanced SEP detection
BlockChain get_block_chain(uhd::rfnoc::rfnoc_graph::sptr graph,
                          const std::string& start_block, 
                          size_t start_port,
                          bool upstream = false) {
    BlockChain chain;
    chain.first_block = start_block;
    chain.first_port = start_port;
    
    // Get all connections
    auto all_edges = graph->enumerate_active_connections();
    auto static_edges = graph->enumerate_static_connections();
    
    // Combine all edges
    all_edges.insert(all_edges.end(), static_edges.begin(), static_edges.end());
    
    // Build chain
    std::string current_block = start_block;
    size_t current_port = start_port;
    std::set<std::string> visited;
    
    // Check if start block has SEP
    auto blocks = discover_blocks(graph);
    for (const auto& block : blocks) {
        if (block.block_id == start_block && block.has_stream_endpoint) {
            chain.has_sep = true;
            break;
        }
    }
    
    while (true) {
        visited.insert(current_block);
        bool found_connection = false;
        
        for (const auto& edge : all_edges) {
            if (upstream) {
                // Looking for blocks that connect TO current block
                if (edge.dst_blockid == current_block && edge.dst_port == current_port) {
                    if (visited.count(edge.src_blockid) == 0) {
                        chain.edges.push_back(edge);
                        current_block = edge.src_blockid;
                        current_port = edge.src_port;
                        found_connection = true;
                        break;
                    }
                }
            } else {
                // Looking for blocks that current block connects TO
                if (edge.src_blockid == current_block && edge.src_port == current_port) {
                    if (visited.count(edge.dst_blockid) == 0) {
                        chain.edges.push_back(edge);
                        current_block = edge.dst_blockid;
                        current_port = edge.dst_port;
                        found_connection = true;
                        break;
                    }
                }
            }
        }
        
        if (!found_connection) {
            // End of chain
            chain.last_block = current_block;
            chain.last_port = current_port;
            break;
        }
        
        // Check if current block has SEP
        for (const auto& block : blocks) {
            if (block.block_id == current_block && block.has_stream_endpoint) {
                chain.has_sep = true;
                chain.last_block = current_block;
                chain.last_port = current_port;
                break;
            }
        }
        
        if (chain.has_sep && !upstream) {
            break; // Stop at first SEP when going downstream
        }
    }
    
    // If going upstream, reverse the edges
    if (upstream) {
        std::reverse(chain.edges.begin(), chain.edges.end());
    }
    
    return chain;
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
        } catch (const uhd::exception& e) {
            std::cerr << "Failed to configure switchboard: " << e.what() << std::endl;
            return false;
        }
    }
    return true;
}

// Find the best streaming endpoint with enhanced logic
std::pair<std::string, size_t> find_best_stream_endpoint(uhd::rfnoc::rfnoc_graph::sptr graph,
                                                        const std::vector<StreamEndpointConfig>& configured_endpoints,
                                                        const std::string& start_block = "") {
    // First check if there are explicitly configured endpoints
    if (!configured_endpoints.empty()) {
        // Return the first RX endpoint
        for (const auto& ep : configured_endpoints) {
            if (ep.direction == "rx") {
                return {ep.block_id, ep.port};
            }
        }
    }
    
    auto blocks = discover_blocks(graph);
    
    // If start_block is specified, find the end of its chain
    if (!start_block.empty()) {
        auto chain = get_block_chain(graph, start_block, 0, false);
        if (chain.has_sep) {
            return {chain.last_block, chain.last_port};
        }
    }
    
    // Priority order for streaming endpoints
    std::vector<std::string> priority_order = {
        "DDC", "DUC", "Radio", "Replay", "DmaFIFO", "SigGen", "Null"
    };
    
    // Check blocks in priority order
    for (const auto& block_type : priority_order) {
        for (const auto& block : blocks) {
            if (block.block_type == block_type) {
                // For DDC/DUC, check if they have downstream connections
                if (block_type == "DDC" || block_type == "DUC") {
                    auto chain = get_block_chain(graph, block.block_id, 0, false);
                    if (chain.edges.empty() || chain.has_sep) {
                        return {block.block_id, 0};
                    }
                } else if (block.has_stream_endpoint) {
                    return {block.block_id, 0};
                }
            }
        }
    }
    
    // Last resort: any block with output ports
    for (const auto& block : blocks) {
        if (block.num_output_ports > 0) {
            return {block.block_id, 0};
        }
    }
    
    return {"", 0};
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
    
    // Analyze and print block chains
    std::cout << "\nBlock chains:" << std::endl;
    std::set<std::string> analyzed;
    for (const auto& block : blocks) {
        if (analyzed.count(block.block_id) == 0 && block.num_input_ports == 0) {
            // Start of a chain
            auto chain = get_block_chain(graph, block.block_id, 0, false);
            if (!chain.edges.empty()) {
                std::cout << "  * Chain starting from " << block.block_id << ":" << std::endl;
                for (const auto& edge : chain.edges) {
                    std::cout << "    " << edge.src_blockid << ":" << edge.src_port 
                             << " -> " << edge.dst_blockid << ":" << edge.dst_port << std::endl;
                    analyzed.insert(edge.src_blockid);
                    analyzed.insert(edge.dst_blockid);
                }
                if (chain.has_sep) {
                    std::cout << "    (Has stream endpoint)" << std::endl;
                }
            }
        }
    }
}

// Write YAML config template
void write_yaml_template(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to create YAML template file");
    }
    
    file << "# RFNoC Graph Configuration Template\n";
    file << "# Generated by CHDR Capture Tool\n\n";
    
    file << "# Dynamic block connections\n";
    file << "connections:\n";
    file << "  - src_block: \"Radio:0\"\n";
    file << "    src_port: 0\n";
    file << "    dst_block: \"DDC:0\"\n";
    file << "    dst_port: 0\n";
    file << "    is_back_edge: false  # Set to true for feedback loops\n\n";
    
    file << "# Switchboard routing configuration\n";
    file << "switchboards:\n";
    file << "  - block_id: \"Switchboard:0\"\n";
    file << "    connections:\n";
    file << "      - input: 0\n";
    file << "        output: 1\n";
    file << "      - input: 1\n";
    file << "        output: 0\n\n";
    
    file << "# Stream endpoint configuration\n";
    file << "stream_endpoints:\n";
    file << "  - block_id: \"DDC:0\"\n";
    file << "    port: 0\n";
    file << "    direction: \"rx\"  # rx or tx\n";
    file << "    stream_args:\n";
    file << "      spp: \"2000\"\n\n";
    
    file << "# Block-specific properties\n";
    file << "block_properties:\n";
    file << "  # Radio block configuration\n";
    file << "  \"Radio:0\":\n";
    file << "    freq: \"100e6\"        # Center frequency\n";
    file << "    gain: \"30\"          # Gain in dB\n";
    file << "    rate: \"10e6\"        # Sample rate\n";
    file << "    antenna: \"RX2\"      # Antenna selection\n";
    file << "    bandwidth: \"20e6\"   # Analog bandwidth\n\n";
    
    file << "  # DDC block configuration\n";
    file << "  \"DDC:0\":\n";
    file << "    freq: \"1e6\"         # DDC frequency offset\n";
    file << "    output_rate: \"1e6\"  # Decimated output rate\n\n";
    
    file << "  # DUC block configuration\n";
    file << "  \"DUC:0\":\n";
    file << "    freq: \"1e6\"         # DUC frequency offset\n";
    file << "    input_rate: \"1e6\"   # Input sample rate\n\n";
    
    file << "  # FIR Filter configuration\n";
    file << "  \"FIR:0\":\n";
    file << "    coeffs: \"1,2,3,4,5,4,3,2,1\"  # Filter coefficients\n\n";
    
    file << "  # FFT block configuration\n";
    file << "  \"FFT:0\":\n";
    file << "    length: \"1024\"      # FFT size\n";
    file << "    magnitude: \"false\"  # Output magnitude (true) or complex (false)\n";
    file << "    direction: \"forward\"  # forward or reverse\n";
    file << "    scaling: \"1706\"     # Scaling factor\n";
    file << "    shift: \"normal\"     # normal, reverse, or natural\n\n";
    
    file << "  # Window block configuration\n";
    file << "  \"Window:0\":\n";
    file << "    coeffs: \"1,2,3,4,5\"  # Window coefficients\n";
    file << "    coeffs/1: \"1,2,3,4\"  # Window coefficients for channel 1\n\n";
    
    file << "  # Signal Generator configuration\n";
    file << "  \"SigGen:0\":\n";
    file << "    enable: \"true\"\n";
    file << "    waveform: \"sine\"    # sine, noise, or constant\n";
    file << "    amplitude: \"0.5\"\n";
    file << "    frequency: \"1000\"   # Frequency in Hz (requires sample rate)\n\n";
    
    file << "  # Replay block configuration\n";
    file << "  # Note: Replay block is configured via API calls, not properties\n";
    file << "  \"Replay:0\":\n";
    file << "    # Use replay API methods for configuration\n\n";
    
    file << "  # Moving Average configuration\n";
    file << "  \"MovingAverage:0\":\n";
    file << "    sum_len: \"10\"       # Averaging length (max 255)\n";
    file << "    divisor: \"10\"       # Divisor\n\n";
    
    file << "  # Vector IIR configuration\n";
    file << "  \"VectorIIR:0\":\n";
    file << "    alpha: \"0.9\"        # IIR alpha coefficient\n";
    file << "    beta: \"0.1\"         # IIR beta coefficient\n\n";
    
    file << "  # Keep One in N configuration\n";
    file << "  \"KeepOneInN:0\":\n";
    file << "    n: \"10\"             # Keep 1 sample every N\n\n";
    
    file << "# Auto-connection settings\n";
    file << "auto_connect:\n";
    file << "  radio_to_ddc: true      # Automatically connect Radio to DDC\n";
    file << "  find_stream_endpoint: true  # Auto-find best streaming point\n\n";
    
    file << "# Advanced configuration options\n";
    file << "advanced:\n";
    file << "  commit_after_each_connection: false  # Commit graph after each connection\n";
    file << "  discover_static_connections: true    # Include static connections in discovery\n";
    file << "  preserve_static_routes: true         # Don't override static connections\n";
    file << "  block_init_order:                    # Specific initialization order\n";
    file << "    - \"Radio:0\"\n";
    file << "    - \"DDC:0\"\n";
    
    file.close();
    std::cout << "YAML template written to: " << filename << std::endl;
}

// Analyze packets and write to CSV
void analyze_packets(const std::vector<chdr_packet_data>& packets, 
                    const std::string& csv_file,
                    double tick_rate) {
    std::ofstream csv(csv_file);
    if (!csv.is_open()) {
        throw std::runtime_error("Failed to open CSV file: " + csv_file);
    }
    
    // Write CSV header
    csv << "packet_num,vc,eob,eov,pkt_type,pkt_type_str,num_mdata,seq_num,length,"
        << "dst_epid,has_timestamp,timestamp_ticks,timestamp_sec,payload_size,"
        << "num_samples,first_4_bytes_hex" << std::endl;
    
    // Analyze each packet
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& pkt = packets[i];
        
        // Basic fields
        csv << i << ","
            << std::hex << "0x" << std::setw(2) << std::setfill('0') << (int)pkt.vc << ","
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
    std::cout << "Analysis complete. Results written to: " << csv_file << std::endl;
}

// Main capture function with dynamic graph setup
template <typename samp_type>
void capture_with_dynamic_graph(
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
    
    // Apply block initialization order if specified
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
    
    // Apply configurations
    if (!config.switchboard_configs.empty()) {
        std::cout << "\nConfiguring switchboards..." << std::endl;
        configure_switchboards(graph, config.switchboard_configs);
    }
    
    if (!config.dynamic_connections.empty()) {
        std::cout << "\nApplying dynamic connections..." << std::endl;
        apply_dynamic_connections(graph, config.dynamic_connections, config.commit_after_each_connection);
    }
    
    if (config.auto_connect_radio_to_ddc) {
        std::cout << "\nAuto-connecting Radio to DDC..." << std::endl;
        auto_connect_radio_to_ddc(graph);
    }
    
    if (!config.block_properties.empty()) {
        std::cout << "\nApplying block properties..." << std::endl;
        apply_block_properties(graph, config.block_properties, rate);
    }
    
    // Commit graph changes
    std::cout << "\nCommitting graph..." << std::endl;
    graph->commit();
    
    // Find best streaming endpoint
    auto [stream_block, stream_port] = find_best_stream_endpoint(graph, config.stream_endpoints);
    if (stream_block.empty()) {
        throw std::runtime_error("No suitable streaming endpoint found");
    }
    
    std::cout << "\nStreaming from: " << stream_block << ":" << stream_port << std::endl;
    
    // Create stream args
    uhd::stream_args_t stream_args("fc32", "sc16");
    stream_args.channels = {0};
    stream_args.args["spp"] = std::to_string(samps_per_buff);
    
    // Apply custom stream args from config
    for (const auto& sep : config.stream_endpoints) {
        if (sep.block_id == stream_block && sep.port == stream_port) {
            for (const auto& [key, value] : sep.stream_args) {
                stream_args.args[key] = value;
            }
            break;
        }
    }
    
    // Create RX streamer and connect to the streaming block
    auto rx_streamer = graph->create_rx_streamer(1, stream_args);
    graph->connect(stream_block, stream_port, rx_streamer, 0);
    graph->commit();
    
    // Get tick rate
    double tick_rate = DEFAULT_TICKRATE;
    auto radio_blocks = graph->find_blocks("Radio");
    if (!radio_blocks.empty()) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_blocks[0]);
        tick_rate = radio->get_tick_rate();
    }
    
    std::cout << "\nStream configuration:" << std::endl;
    std::cout << "  Sample rate: " << rate/1e6 << " Msps" << std::endl;
    std::cout << "  Tick rate: " << tick_rate/1e6 << " MHz" << std::endl;
    std::cout << "  Samples per buffer: " << samps_per_buff << std::endl;
    
    // Print final graph state
    std::cout << "\n=== Final Graph State ===" << std::endl;
    print_graph_info(graph);
    
    // Open output file
    std::ofstream outfile(file.c_str(), std::ios::binary);
    if (!outfile.is_open()) {
        throw std::runtime_error("Failed to open file: " + file);
    }
    
    // Write file header
    struct {
        uint32_t magic;
        uint32_t version;
        uint32_t chdr_width;
        double tick_rate;
    } file_header;
    
    file_header.magic = 0x43484452; // "CHDR"
    file_header.version = 2;
    file_header.chdr_width = 64;
    file_header.tick_rate = tick_rate;
    
    outfile.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    
    // Setup streaming
    std::vector<samp_type> buff(samps_per_buff);
    std::vector<void*> buff_ptrs = {&buff.front()};
    uhd::rx_metadata_t md;
    
    // Start streaming
    uhd::stream_cmd_t stream_cmd(num_packets == 0 ? 
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS :
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    
    if (num_packets > 0) {
        stream_cmd.num_samps = num_packets * samps_per_buff;
    }
    
    stream_cmd.stream_now = true;
    rx_streamer->issue_stream_cmd(stream_cmd);
    
    // Statistics
    size_t packets_captured = 0;
    size_t total_samples = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // For analysis
    std::vector<chdr_packet_data> captured_packets;
    
    std::cout << "\nStarting capture..." << std::endl;
    
    // Main capture loop
    while (!stop_signal_called && (num_packets == 0 || packets_captured < num_packets)) {
        size_t num_rx_samps = rx_streamer->recv(buff_ptrs, samps_per_buff, md, 3.0);
        
        // Handle errors
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << "Timeout while streaming" << std::endl;
            break;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            std::cerr << "O" << std::flush;
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "Receiver error: " << md.strerror() << std::endl;
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
        header |= (uint64_t)0 & 0xFFFF;                          // EPID
        header |= ((uint64_t)total_chdr_bytes & 0xFFFF) << 16;   // Length
        header |= ((uint64_t)packets_captured & 0xFFFF) << 32;   // Sequence
        header |= ((uint64_t)0 & 0x1F) << 48;                    // NumMData
        header |= ((uint64_t)(md.has_time_spec ? PKT_TYPE_DATA_WITH_TS : PKT_TYPE_DATA_NO_TS) & 0x7) << 53;
        header |= ((uint64_t)(md.end_of_burst ? 1 : 0) & 0x1) << 57; // EOB
        
        // Write header
        write_le(packet_buffer, header);
        
        // Write timestamp if present
        if (md.has_time_spec) {
            uint64_t timestamp_ticks = md.time_spec.to_ticks(tick_rate);
            write_le(packet_buffer, timestamp_ticks);
        }
        
        // Write payload
        const uint8_t* sample_bytes = reinterpret_cast<const uint8_t*>(buff.data());
        packet_buffer.insert(packet_buffer.end(), sample_bytes, sample_bytes + payload_bytes);
        
        // Write to file
        uint32_t pkt_size = static_cast<uint32_t>(packet_buffer.size());
        outfile.write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
        outfile.write(reinterpret_cast<const char*>(packet_buffer.data()), packet_buffer.size());
        
        // Store for analysis
        if (enable_analysis && captured_packets.size() < 10000) {
            chdr_packet_data pkt;
            pkt.header_raw = header;
            pkt.parse_header();
            
            if (md.has_time_spec) {
                pkt.timestamp = md.time_spec.to_ticks(tick_rate);
            }
            pkt.payload.assign(sample_bytes, sample_bytes + payload_bytes);
            captured_packets.push_back(pkt);
        }
        
        // Update statistics
        packets_captured++;
        total_samples += num_rx_samps;
        
        // Progress update
        if (packets_captured % 100 == 0) {
            std::cout << "\rPackets: " << packets_captured 
                      << ", Samples: " << total_samples << std::flush;
        }
    }
    
    std::cout << std::endl;
    outfile.close();
    
    // Stop streaming
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_streamer->issue_stream_cmd(stream_cmd);
    
    // Final statistics
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    double elapsed_secs = std::chrono::duration<double>(elapsed).count();
    
    std::cout << "\n=== Capture Statistics ===" << std::endl;
    std::cout << "Total packets captured: " << packets_captured << std::endl;
    std::cout << "Total samples: " << total_samples << std::endl;
    std::cout << "Capture duration: " << elapsed_secs << " seconds" << std::endl;
    std::cout << "Average sample rate: " << (total_samples/elapsed_secs)/1e6 << " Msps" << std::endl;
    
    // Perform analysis
    if (enable_analysis && !csv_file.empty() && !captured_packets.empty()) {
        std::cout << "\nAnalyzing packets..." << std::endl;
        analyze_packets(captured_packets, csv_file, tick_rate);
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
    
    // Setup program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "UHD device arguments")
        ("file", po::value<std::string>(&file)->default_value("chdr_capture.dat"), "output filename")
        ("yaml", po::value<std::string>(&yaml_config)->default_value(""), "YAML configuration file")
        ("csv", po::value<std::string>(&csv_file)->default_value(""), "CSV output file for analysis")
        ("num-packets", po::value<size_t>(&num_packets)->default_value(1000), "number of packets to capture (0 for continuous)")
        ("rate", po::value<double>(&rate)->default_value(10e6), "sample rate")
        ("freq", po::value<double>(&freq)->default_value(100e6), "center frequency")
        ("gain", po::value<double>(&gain)->default_value(30.0), "gain")
        ("bw", po::value<double>(&bw)->default_value(0.0), "analog bandwidth")
        ("format", po::value<std::string>(&format)->default_value("sc16"), "sample format")
        ("spb", po::value<size_t>(&spb)->default_value(2000), "samples per buffer")
        ("show-graph", po::value<bool>(&show_graph)->default_value(false), "print RFNoC graph info and exit")
        ("analyze-only", po::value<bool>(&analyze_only)->default_value(false), "only analyze existing file")
        ("create-yaml-template", po::value<bool>(&create_yaml_template)->default_value(false), "create YAML configuration template")
    ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    // Help message
    if (vm.count("help")) {
        std::cout << "CHDR Packet Capture Tool with Full RFNoC Block Support" << std::endl;
        std::cout << desc << std::endl;
        std::cout << "\nSupported RFNoC blocks:" << std::endl;
        std::cout << "  * Radio - RF frontend control" << std::endl;
        std::cout << "  * DDC/DUC - Digital down/up converters" << std::endl;
        std::cout << "  * FIR - FIR filter" << std::endl;
        std::cout << "  * FFT - Fast Fourier Transform" << std::endl;
        std::cout << "  * Window - Windowing function" << std::endl;
        std::cout << "  * SigGen - Signal generator" << std::endl;
        std::cout << "  * Replay - Record/playback buffer" << std::endl;
        std::cout << "  * Switchboard - Dynamic routing" << std::endl;
        std::cout << "  * MovingAverage - Moving average filter" << std::endl;
        std::cout << "  * VectorIIR - Vector IIR filter" << std::endl;
        std::cout << "  * KeepOneInN - Decimator" << std::endl;
        std::cout << "  * AddSub - Add/subtract blocks" << std::endl;
        std::cout << "  * SplitStream - Stream splitter" << std::endl;
        std::cout << "  * Null - Null source/sink" << std::endl;
        std::cout << "\nNotes:" << std::endl;
        std::cout << "  * Some blocks (like Replay) are configured via API calls, not properties" << std::endl;
        std::cout << "  * Use --show-graph to see available blocks and connections in your device" << std::endl;
        std::cout << "  * Use --create-yaml-template to generate a comprehensive YAML config template" << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Create YAML template if requested
    if (create_yaml_template) {
        write_yaml_template("rfnoc_config_template.yaml");
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
        
        // Add default Radio properties if Radio block exists
        auto radio_blocks = graph->find_blocks("Radio");
        if (!radio_blocks.empty()) {
            std::string radio_id = radio_blocks[0].to_string();
            config.block_properties[radio_id]["freq"] = std::to_string(freq);
            config.block_properties[radio_id]["gain"] = std::to_string(gain);
            config.block_properties[radio_id]["rate"] = std::to_string(rate);
            if (bw > 0) {
                config.block_properties[radio_id]["bandwidth"] = std::to_string(bw);
            }
        }
    }
    
    // Start capture with dynamic graph setup
    bool enable_analysis = !csv_file.empty();
    
    if (format == "sc16") {
        capture_with_dynamic_graph<std::complex<short>>(
            graph, config, file, num_packets, enable_analysis, csv_file, rate, spb);
    } else if (format == "fc32") {
        capture_with_dynamic_graph<std::complex<float>>(
            graph, config, file, num_packets, enable_analysis, csv_file, rate, spb);
    } else {
        throw std::runtime_error("Unsupported format: " + format);
    }
    
    return EXIT_SUCCESS;
}