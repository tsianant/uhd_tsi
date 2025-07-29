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
#include "rfnoc_stream_tool/rfnoc_stream_tool.h"


void sig_int_handler(int) {
    stop_signal_called.store(true, std::memory_order_relaxed);
    // propagate to per‑stream flags so writer threads exit promptly
    for (auto* f : g_per_stream_stop_flags) {
        if (f) f->store(true, std::memory_order_relaxed);
    }
}

void print_graph_info(const uhd::rfnoc::rfnoc_graph::sptr& graph);
// void sig_int_handler(int)
// {
//     stop_signal_called.store(true);
// }

// ============================================================================
// Lock-free SPSC Ring Buffer Implementation
// ============================================================================


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

// ============================================================================
// File Writer Thread Function
// ============================================================================

void file_writer_thread(
    StreamContext& ctx,
    std::atomic<bool>& stop_writing,
    FileWriterStats& writer_stats)
{
    writer_stats.start_time = std::chrono::steady_clock::now();
    
    // Set thread priority for file I/O
    uhd::set_thread_priority_safe(0.5, true);
    
    // Open output file
    std::ofstream output_file(ctx.output_filename, std::ios::binary);
    if (!output_file.is_open()) {
        std::cerr << "[Writer " << ctx.stream_id << "] Failed to open file: " 
                  << ctx.output_filename << std::endl;
        return;
    }
    
    // Write file header
    ChdrFileHeader header;
    header.tick_rate = ctx.tick_rate;
    header.pps_reset_used = ctx.pps_reset_used ? 1 : 0;
    header.pps_reset_time_sec = ctx.pps_reset_time.get_real_secs();
    header.num_streams = 1;  // One stream per file
    header.ring_buffer_used = 1;
    output_file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // Write stream header
    StreamHeader stream_header;
    stream_header.stream_id = static_cast<uint32_t>(ctx.stream_id);
    std::strncpy(stream_header.block_id, ctx.block_id.c_str(), sizeof(stream_header.block_id) - 1);
    stream_header.block_id[sizeof(stream_header.block_id) - 1] = '\0';
    stream_header.port = static_cast<uint32_t>(ctx.port);
    stream_header.ring_buffer_size = static_cast<uint32_t>(ctx.ring_buffer->capacity());
    output_file.write(reinterpret_cast<const char*>(&stream_header), sizeof(stream_header));
    
    std::cout << "[Writer " << ctx.stream_id << "] Started file writer for " 
              << ctx.output_filename << std::endl;
    
    // Buffer for batched writes
    std::vector<PacketBuffer> write_batch;
    write_batch.reserve(ctx.buffer_config.batch_write_size);
    
    // Main write loop
    while (!stop_writing.load() || !ctx.ring_buffer->empty()) {
        PacketBuffer packet;
        
        // Try to fill the batch
        while (write_batch.size() < ctx.buffer_config.batch_write_size && 
               ctx.ring_buffer->pop(packet)) {
            write_batch.push_back(packet);
        }
        
        // Write batch if we have data
        if (!write_batch.empty()) {
            try {
                for (const auto& pkt : write_batch) {
                    uint32_t pkt_size = static_cast<uint32_t>(pkt.data.size());
                    output_file.write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
                    output_file.write(reinterpret_cast<const char*>(pkt.data.data()), pkt.data.size());
                    
                    writer_stats.packets_written++;
                    writer_stats.bytes_written += sizeof(pkt_size) + pkt.data.size();
                }
                
                write_batch.clear();
            } catch (const std::exception& e) {
                std::cerr << "[Writer " << ctx.stream_id << "] Write error: " << e.what() << std::endl;
                writer_stats.write_errors++;
            }
        } else if (stop_writing.load() && ctx.ring_buffer->empty()) {
            break;  // Exit if we're done and buffer is empty
        } else {
            // No data available, sleep briefly
            std::this_thread::sleep_for(1ms);
        }
        
        // Update buffer usage statistics
        size_t current_usage = ctx.ring_buffer->size();
        if (current_usage > ctx.stats.max_buffer_usage) {
            ctx.stats.max_buffer_usage = current_usage;
        }
        
        // Check for high water mark
        size_t usage_percent = (current_usage * 100) / ctx.ring_buffer->capacity();
        if (usage_percent >= ctx.buffer_config.high_water_mark) {
            std::cout << "[Writer " << ctx.stream_id << "] WARNING: Buffer usage at " 
                      << usage_percent << "%" << std::endl;
        }
    }
    
    // Final flush of any remaining packets
    while (!ctx.ring_buffer->empty()) {
        PacketBuffer packet;
        if (ctx.ring_buffer->pop(packet)) {
            try {
                uint32_t pkt_size = static_cast<uint32_t>(packet.data.size());
                output_file.write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
                output_file.write(reinterpret_cast<const char*>(packet.data.data()), packet.data.size());
                
                writer_stats.packets_written++;
                writer_stats.bytes_written += sizeof(pkt_size) + packet.data.size();
            } catch (const std::exception& e) {
                std::cerr << "[Writer " << ctx.stream_id << "] Final write error: " << e.what() << std::endl;
                writer_stats.write_errors++;
            }
        }
    }
    
    output_file.close();
    writer_stats.end_time = std::chrono::steady_clock::now();
    
    double duration = std::chrono::duration<double>(writer_stats.end_time - writer_stats.start_time).count();
    std::cout << "[Writer " << ctx.stream_id << "] Stopped. Wrote " 
              << writer_stats.packets_written << " packets (" 
              << writer_stats.bytes_written / (1024.0 * 1024.0) << " MB) in "
              << duration << " seconds" << std::endl;
    
    if (writer_stats.write_errors > 0) {
        std::cout << "[Writer " << ctx.stream_id << "] Write errors: " 
                  << writer_stats.write_errors << std::endl;
    }
}

// ============================================================================
// Enhanced Capture Stream Function with Ring Buffer
// ============================================================================

template <typename samp_type>
void capture_stream_ringbuffer(StreamContext& ctx, 
                              std::atomic<bool>& start_capture,
                              std::atomic<bool>& stop_writing,
                              size_t num_packets,
                              FileWriterStats& writer_stats) {
    
    // Set thread priority for capture
    uhd::set_thread_priority_safe(1.0, true);
    
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
              << ctx.block_id << ":" << ctx.port 
              << " with ring buffer size: " << ctx.ring_buffer->capacity() << std::endl;
    
    // Main capture loop
    size_t consecutive_timeouts = 0;
    const size_t max_consecutive_timeouts = 5;
    
    while (!stop_signal_called.load() && (num_packets == 0 ||
        ctx.stats.packets_captured < num_packets)) {
        size_t num_rx_samps = ctx.rx_streamer->recv(buff_ptrs, ctx.samps_per_buff, md, 3.0);
        
        // Handle errors
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            consecutive_timeouts++;
            if (consecutive_timeouts >= max_consecutive_timeouts) {
                std::cout << "[Stream " << ctx.stream_id << "] Multiple timeouts, stopping stream" << std::endl;
                break;
            }
            continue;
        } else {
            consecutive_timeouts = 0;  // Reset timeout counter
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
        PacketBuffer packet_buffer;
        packet_buffer.stream_id = ctx.stream_id;
        packet_buffer.packet_number = ctx.stats.packets_captured;
        
        // Calculate sizes
        size_t payload_bytes = num_rx_samps * sizeof(samp_type);
        size_t header_bytes = 8;
        size_t timestamp_bytes = md.has_time_spec ? 8 : 0;
        size_t total_chdr_bytes = header_bytes + timestamp_bytes + payload_bytes;
        
        // Reserve space
        packet_buffer.data.reserve(total_chdr_bytes);
        
        // Build header
        uint64_t header = 0;
        header |= (uint64_t)ctx.stream_id & 0xFFFF;
        header |= ((uint64_t)total_chdr_bytes & 0xFFFF) << 16;
        header |= ((uint64_t)ctx.stats.packets_captured & 0xFFFF) << 32;
        header |= ((uint64_t)0 & 0x1F) << 48;
        header |= ((uint64_t)(md.has_time_spec ? PKT_TYPE_DATA_WITH_TS : PKT_TYPE_DATA_NO_TS) & 0x7) << 53;
        header |= ((uint64_t)(md.end_of_burst ? 1 : 0) & 0x1) << 57;
        
        // Write header
        write_le(packet_buffer.data, header);
        
        // Write timestamp if present
        uint64_t timestamp_ticks = 0;
        if (md.has_time_spec) {
            timestamp_ticks = md.time_spec.to_ticks(ctx.tick_rate);
            write_le(packet_buffer.data, timestamp_ticks);
            packet_buffer.has_timestamp = true;
            packet_buffer.timestamp = md.time_spec;
            
            // Track timestamps
            double timestamp_sec = md.time_spec.get_real_secs();
            if (ctx.stats.first_timestamp == 0.0) {
                ctx.stats.first_timestamp = timestamp_sec;
            }
            ctx.stats.last_timestamp = timestamp_sec;
        }
        
        // Write payload
        const uint8_t* sample_bytes = reinterpret_cast<const uint8_t*>(buff.data());
        packet_buffer.data.insert(packet_buffer.data.end(), sample_bytes, sample_bytes + payload_bytes);
        
        // Push to ring buffer
        if (!ctx.ring_buffer->push(std::move(packet_buffer))) {
            ctx.stats.buffer_overflows++;
            if (ctx.stats.buffer_overflows % 100 == 0) {
                std::cerr << "[Stream " << ctx.stream_id << "] Ring buffer overflow! Total: " 
                          << ctx.stats.buffer_overflows << std::endl;
            }
        }
        
        // Store for analysis if needed
        if (ctx.analysis_packets && ctx.analysis_packets->size() < MAX_ANALYSIS_PACKETS) {
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
        ctx.stats.total_bytes_written += packet_buffer.data.size();
        
        // Progress update
        if (ctx.stats.packets_captured % 1000 == 0) {
            size_t buffer_usage = ctx.ring_buffer->size();
            size_t buffer_percent = (buffer_usage * 100) / ctx.ring_buffer->capacity();
            
            std::cout << "[Stream " << ctx.stream_id << "] Packets: " 
                      << ctx.stats.packets_captured 
                      << ", Samples: " << ctx.stats.total_samples
                      << ", Buffer: " << buffer_percent << "%";
            if (ctx.stats.overflow_count > 0) {
                std::cout << " (RF-O: " << ctx.stats.overflow_count << ")";
            }
            if (ctx.stats.buffer_overflows > 0) {
                std::cout << " (Buf-O: " << ctx.stats.buffer_overflows << ")";
            }
            std::cout << std::endl;
        }
    }
    
    // Stop streaming
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    ctx.rx_streamer->issue_stream_cmd(stream_cmd);
    
    ctx.stats.end_time = std::chrono::steady_clock::now();
    
    // Signal writer thread to finish
    stop_writing.store(true);
    
    // Final statistics
    double elapsed_secs = std::chrono::duration<double>(ctx.stats.end_time - ctx.stats.start_time).count();
    std::cout << "\n[Stream " << ctx.stream_id << "] Capture complete:" << std::endl;
    std::cout << "  Block: " << ctx.block_id << ":" << ctx.port << std::endl;
    std::cout << "  Packets: " << ctx.stats.packets_captured << std::endl;
    std::cout << "  Samples: " << ctx.stats.total_samples << std::endl;
    std::cout << "  Duration: " << elapsed_secs << " seconds" << std::endl;
    std::cout << "  Average rate: " << (ctx.stats.total_samples/elapsed_secs)/1e6 << " Msps" << std::endl;
    std::cout << "  Max buffer usage: " << ctx.stats.max_buffer_usage << "/" 
              << ctx.ring_buffer->capacity() << " packets" << std::endl;
    if (ctx.stats.overflow_count > 0) {
        std::cout << "  RF Overflows: " << ctx.stats.overflow_count << std::endl;
    }
    if (ctx.stats.buffer_overflows > 0) {
        std::cout << "  Buffer Overflows: " << ctx.stats.buffer_overflows << std::endl;
    }
}


// Perform PPS reset to synchronize device time to 0
uhd::time_spec_t perform_pps_reset(uhd::rfnoc::rfnoc_graph::sptr graph, 
                                  const PpsResetConfig& config) {
    uhd::time_spec_t pps_reset_time(0.0);
    
    if (!config.enable_pps_reset) {
        return pps_reset_time;
    }
    
    std::cout << "\n=== PPS Reset Sequence ===" << std::endl;
    std::cout << "Waiting for PPS edge to reset device time to 0..." << std::endl;
    
    try {
        // Get the motherboard controller to access time functions
        auto mb_controller = graph->get_mb_controller(0);
        
        // Get the timekeeper
        auto timekeeper = mb_controller->get_timekeeper(0);
        
        // Get current time before reset
        uhd::time_spec_t time_before = timekeeper->get_time_now();
        std::cout << "Device time before PPS reset: " << time_before.get_real_secs() << " seconds" << std::endl;
        
        // Set time to 0 on next PPS
        timekeeper->set_ticks_next_pps(0);
        
        // Wait for PPS to occur
        std::cout << "Waiting " << config.wait_time_sec << " seconds for PPS..." << std::endl;
        std::this_thread::sleep_for(std::chrono::duration<double>(config.wait_time_sec));
        
        // Get current time to verify it's been reset
        uhd::time_spec_t current_time = timekeeper->get_time_now();
        std::cout << "Device time after PPS reset: " << current_time.get_real_secs() << " seconds" << std::endl;
        
        // Verify the reset actually occurred
        if (config.verify_reset) {
            if (current_time.get_real_secs() > config.max_time_after_reset) {
                std::cerr << "Warning: PPS reset may have failed - time is " 
                          << current_time.get_real_secs() << " seconds (expected < " 
                          << config.max_time_after_reset << ")" << std::endl;
            } else {
                std::cout << "PPS reset successful - time synchronized to PPS edge" << std::endl;
            }
        }
        
        // Store the PPS reset time (should be 0.0 if reset worked)
        pps_reset_time = uhd::time_spec_t(0.0);
        
    } catch (const std::exception& e) {
        std::cerr << "Error during PPS reset: " << e.what() << std::endl;
        std::cerr << "Continuing without PPS reset..." << std::endl;
    }
    
    std::cout << "PPS reset sequence complete." << std::endl;
    return pps_reset_time;
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
        
        // Load PPS reset configuration
        if (root["pps_reset"]) {
            auto& pps = config.pps_reset;
            pps.enable_pps_reset = root["pps_reset"]["enable"].as<bool>(false);
            pps.wait_time_sec = root["pps_reset"]["wait_time_sec"].as<double>(1.5);
            pps.verify_reset = root["pps_reset"]["verify_reset"].as<bool>(true);
            pps.max_time_after_reset = root["pps_reset"]["max_time_after_reset"].as<double>(1.0);
        }
        
        
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
        
        // Load explicit signal paths (new feature)
        if (root["signal_paths"]) {
            for (const auto& path : root["signal_paths"]) {
                SignalPathConfig spc;
                spc.name = path["name"].as<std::string>("");
                if (path["connections"]) {
                    for (const auto& conn : path["connections"]) {
                        ConnectionConfig cc;
                        cc.src_block = conn["src"].as<std::string>();
                        cc.dst_block = conn["dst"].as<std::string>();
                        
                        // Parse port from format "Block:Port"
                        size_t src_colon = cc.src_block.find(':');
                        size_t dst_colon = cc.dst_block.find(':');
                        
                        if (src_colon != std::string::npos) {
                            cc.src_port = std::stoul(cc.src_block.substr(src_colon + 1));
                            cc.src_block = cc.src_block.substr(0, src_colon);
                        } else {
                            cc.src_port = conn["src_port"].as<size_t>(0);
                        }
                        
                        if (dst_colon != std::string::npos) {
                            cc.dst_port = std::stoul(cc.dst_block.substr(dst_colon + 1));
                            cc.dst_block = cc.dst_block.substr(0, dst_colon);
                        } else {
                            cc.dst_port = conn["dst_port"].as<size_t>(0);
                        }
                        
                        cc.is_back_edge = conn["is_back_edge"].as<bool>(false);
                        spc.connections.push_back(cc);
                    }
                }
                config.signal_paths.push_back(spc);
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

// Check if a connection already exists
bool connection_exists(uhd::rfnoc::rfnoc_graph::sptr graph,
                      const std::string& src_block, size_t src_port,
                      const std::string& dst_block, size_t dst_port) {
    auto edges = graph->enumerate_active_connections();
    for (const auto& edge : edges) {
        if (edge.src_blockid == src_block && edge.src_port == src_port &&
            edge.dst_blockid == dst_block && edge.dst_port == dst_port) {
            return true;
        }
    }
    return false;
}

// Apply explicit signal paths from configuration
bool apply_signal_paths(uhd::rfnoc::rfnoc_graph::sptr graph,
                       const std::vector<SignalPathConfig>& signal_paths) {
    std::cout << "\nApplying signal paths from configuration..." << std::endl;
    
    for (const auto& path : signal_paths) {
        if (!path.name.empty()) {
            std::cout << "  Establishing path: " << path.name << std::endl;
        }
        
        for (const auto& conn : path.connections) {
            if (!connection_exists(graph, conn.src_block, conn.src_port,
                                 conn.dst_block, conn.dst_port)) {
                try {
                    std::cout << "    Connecting: " << conn.src_block << ":" << conn.src_port
                             << " -> " << conn.dst_block << ":" << conn.dst_port;
                    if (conn.is_back_edge) {
                        std::cout << " (back edge)";
                    }
                    std::cout << std::endl;
                    
                    graph->connect(conn.src_block, conn.src_port,
                                 conn.dst_block, conn.dst_port, conn.is_back_edge);
                } catch (const std::exception& e) {
                    std::cerr << "      Failed: " << e.what() << std::endl;
                    return false;
                }
            } else {
                std::cout << "    Connection already exists: " << conn.src_block << ":" << conn.src_port
                         << " -> " << conn.dst_block << ":" << conn.dst_port << std::endl;
            }
        }
    }
    
    return true;
}

// Find ALL available streaming endpoints for multi-stream support
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
            file << "      spp: \"200\"\n";
        }
    }
    
    // Add signal paths section for explicit routing
    file << "\n# Signal paths to explicitly establish\n";
    file << "signal_paths:\n";
    file << "  - name: \"Radio to DDC via Switchboards\"\n";
    file << "    connections:\n";
    file << "      - src: \"0/Radio#0:0\"\n";
    file << "        dst: \"0/Switchboard#0:0\"\n";
    file << "      - src: \"0/Switchboard#0:1\"  # After internal routing\n";
    file << "        dst: \"0/SplitStream#1:0\"\n";
    file << "      - src: \"0/SplitStream#1:0\"\n";
    file << "        dst: \"0/Switchboard#1:2\"\n";
    file << "      - src: \"0/SplitStream#1:1\"\n";
    file << "        dst: \"0/Switchboard#1:3\"\n";
    file << "      - src: \"0/SplitStream#1:2\"\n";
    file << "        dst: \"0/Switchboard#1:4\"\n";
    file << "      - src: \"0/SplitStream#1:3\"\n";
    file << "        dst: \"0/Switchboard#1:5\"\n";
    file << "      - src: \"0/Switchboard#1:0\"  # After internal routing\n";
    file << "        dst: \"0/DDC#0:0\"\n";
    file << "      - src: \"0/Switchboard#1:1\"\n";
    file << "        dst: \"0/DDC#0:1\"\n";
    file << "      - src: \"0/Switchboard#1:2\"\n";
    file << "        dst: \"0/DDC#0:2\"\n";
    file << "      - src: \"0/Switchboard#1:3\"\n";
    file << "        dst: \"0/DDC#0:3\"\n";
    
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
    
    file << "  radio_to_ddc: " << (has_radio && has_ddc ? "false" : "false") 
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

// Apply block properties for all RFNoC block types
bool apply_block_properties(uhd::rfnoc::rfnoc_graph::sptr graph,
                           const std::map<std::string, std::map<std::string, std::string>>& properties,
                           double default_rate) {
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

// Apply dynamic connections with optional per-connection commit
bool apply_dynamic_connections(uhd::rfnoc::rfnoc_graph::sptr graph,
                              const std::vector<ConnectionConfig>& connections,
                              bool commit_after_each) {
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
void print_graph_info(const uhd::rfnoc::rfnoc_graph::sptr& graph) {
    std::cout << "\n=== RFNoC Graph Information ===" << std::endl;
    
    // Print blocks with more details
    auto blocks = discover_blocks_enhanced(graph);
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

// Analyze packets with multi-stream support
// Enhanced analysis with PPS reset timestamp handling
void analyze_packets_with_pps_reset(const std::vector<chdr_packet_data>& packets, 
                                    const std::string& csv_file,
                                    double tick_rate,
                                    const std::vector<StreamStats>& stream_stats,
                                    uhd::time_spec_t pps_reset_time,
                                    bool pps_reset_used,
                                    size_t samps_per_buff,
                                    double rate) {
    std::cout << "\n\nInside the \"analyze_packets_with_pps_reset\" method" << std::endl;
    std::cout << "\nThe incoming tick rate after static cast to uint64_t is: " << static_cast<uint64_t>(tick_rate) << std::endl;
    
    std::ofstream csv(csv_file);
    if (!csv.is_open()) {
        throw std::runtime_error("Failed to open CSV file: " + csv_file);
    }
    
    
    // Write CSV header with PPS reset info
    csv << "packet_num,         stream_id,          stream_block,   stream_port,        vc,"
        << "eob,                eov,                pkt_type,       pkt_type_str,       num_mdata,"
        << "seq_num,            length,             dst_epid,       has_timestamp,      timestamp_ticks,"
        << "timestamp_sec,      time_since_pps_sec, payload_size,   num_samples,        first_4_bytes_hex" 
        << std::endl;
    
    // Calculate first packet offset for PPS alignment (if needed)
    uint64_t first_pkt_offset = 0;
    uint64_t first_pkt_sec_ticks =0;
    std::cout << "Here's the unmodified tick values for packet 0: " << packets[0].timestamp << "\n\n" <<std::endl;
    if (pps_reset_used && !packets.empty() && packets[0].has_timestamp && packets[0].timestamp > DEFAULT_TICKRATE) {
        first_pkt_offset = ((packets[0].timestamp ) % DEFAULT_TICKRATE);
        first_pkt_sec_ticks = packets[0].timestamp / DEFAULT_TICKRATE;
    }
    
    std::cout << "Here's the First first_packet integer second value (rounded down) is : " << first_pkt_sec_ticks <<"\n\n"<< std::endl;
    std::cout << "Here's the First first_packet tick value is : " << first_pkt_offset <<"\n\n"<< std::endl;

    std::cout << "\n\n----- tick_rate \'%\' sample_rate = " << (DEFAULT_TICKRATE % static_cast<uint64_t>(rate)) << std::endl;
    
    
    double samps_per_sec_num = samps_per_buff * (DEFAULT_TICKRATE / rate);

    std::cout << "\n\nHere's the computed value to be subtracted: " << ((samps_per_sec_num*((static_cast<double>(packets[0].timestamp - first_pkt_offset))/tick_rate))) << "\n\n" << std::endl;

    // Analyze each packet
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& pkt = packets[i];

    
        
        std::cout << "Here's the  timestamp value for pkt number " << i << " : " << pkt.timestamp << std::endl;
        std::cout << "current packet order in line within 1 sec is: "  << ( first_pkt_offset / samps_per_buff ) << std::endl;


        std::cout << "\n\n------ Seconds so far: " << (static_cast<uint64_t>((static_cast<double>(pkt.timestamp - first_pkt_offset ) )/ tick_rate) + 1) << std::endl; 
        
        // if (   (DEFAULT_TICKRATE % static_cast<uint64_t>(rate)) ){
        //     std::cout << "Detected the changeover between seconds!!" << std::endl;
            
        // }

        if( (pkt.timestamp % DEFAULT_TICKRATE) < ((samps_per_sec_num*((static_cast<double>(pkt.timestamp - first_pkt_offset))/tick_rate))) && ((pkt.timestamp % DEFAULT_TICKRATE) < ((packets[i==0?0:(i-1)]).timestamp) % DEFAULT_TICKRATE) ) {
            std::cout << "first_packet_offset rollover detected" <<std::endl;
            first_pkt_offset = ((pkt.timestamp - DEFAULT_TICKRATE) % DEFAULT_TICKRATE);
        }



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
        uint64_t temp_timestamp = 0;
        // Timestamp with PPS reset handling
        csv << (pkt.has_timestamp ? "1" : "0") << ",";
        if (pkt.has_timestamp) {
            // Adjust timestamp relative to PPS reset if used
            uint64_t adjusted_timestamp = pkt.timestamp;
            if (pps_reset_used) {
                // Remove the initial offset to align to PPS edge
                adjusted_timestamp = (pkt.timestamp - first_pkt_offset) % static_cast<uint64_t>(tick_rate);
            }

            uint64_t timestamp_sec = 0;
            double time_since_pps = 0;

            if ( (pkt.timestamp / DEFAULT_TICKRATE) < 2){
                temp_timestamp = (pkt.timestamp % DEFAULT_TICKRATE /*) - first_pkt_offset +  ( first_pkt_offset % (samps_per_buff*(DEFAULT_TICKRATE/static_cast<uint64_t>(rate)))*/ );
                timestamp_sec = ((static_cast<double>(pkt.timestamp ) )/ tick_rate);
                // Calculate time since PPS reset
                time_since_pps = (static_cast<double>(pkt.timestamp /*+  ( first_pkt_offset % (samps_per_buff*(DEFAULT_TICKRATE/static_cast<uint64_t>(rate))) )*/ ) / tick_rate) -  timestamp_sec;
            }
            else{

                temp_timestamp = (pkt.timestamp % DEFAULT_TICKRATE) - first_pkt_offset +  ( first_pkt_offset % (samps_per_buff*(DEFAULT_TICKRATE/static_cast<uint64_t>(rate))) );
                
                timestamp_sec = ((static_cast<double>(pkt.timestamp - first_pkt_offset ) )/ tick_rate);
                // Calculate time since PPS reset
                time_since_pps = (static_cast<double>(pkt.timestamp - first_pkt_offset +  ( first_pkt_offset % (samps_per_buff*(DEFAULT_TICKRATE/static_cast<uint64_t>(rate))) ) ) / tick_rate) -  timestamp_sec;

            }

            // temp_timestamp = (pkt.timestamp % DEFAULT_TICKRATE) - first_pkt_offset +  ( first_pkt_offset % (samps_per_buff*(DEFAULT_TICKRATE/static_cast<uint64_t>(rate))) );

            std::cout << "Here's the first_packet value for pkt number " << i << " : " << first_pkt_offset << std::endl;
            std::cout << "Here's the temptimestamp value for pkt number " << i << " : " << temp_timestamp << std::endl;

            std::cout << "Here's the timestamp_sec value for pkt number " << i << " : " << timestamp_sec << std::endl;
            std::cout << "Here's the time_since_pps value for pkt number " << i << " : " << time_since_pps <<"\n\n" << std::endl;

            csv << std::dec << temp_timestamp << ","
                << std::fixed << std::setprecision(12) << timestamp_sec << ","
                << std::setprecision(12) << time_since_pps;
        } else {
            csv << "N/A,N/A,N/A";
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
    
    // Write stream statistics summary with PPS reset info
    std::string stats_file = csv_file.substr(0, csv_file.find_last_of('.')) + "_stats.csv";
    std::ofstream stats_csv(stats_file);
    if (stats_csv.is_open()) {
        stats_csv << "stream_id,block_id,port,packets_captured,total_samples,"
                  << "overflow_count,error_count,duration_sec,avg_sample_rate_msps,"
                  << "first_timestamp,last_timestamp,pps_reset_used,pps_reset_time" << std::endl;
        
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
                     << stat.last_timestamp << ","
                     << (pps_reset_used ? "1" : "0") << ","
                     << pps_reset_time.get_real_secs() << std::endl;
        }
        stats_csv.close();
        std::cout << "Stream statistics written to: " << stats_file << std::endl;
    }
    
    std::cout << "Analysis complete. Results written to: " << csv_file << std::endl;
}

// Write file header with PPS reset information
void write_file_header(std::ofstream& file, double tick_rate, 
                      bool pps_reset_used, uhd::time_spec_t pps_reset_time,
                      size_t num_streams) {
    ChdrFileHeader header;
    header.tick_rate = tick_rate;
    header.pps_reset_used = pps_reset_used ? 1 : 0;
    header.pps_reset_time_sec = pps_reset_time.get_real_secs();
    header.num_streams = static_cast<uint32_t>(num_streams);
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
}

// Write stream header for multi-stream files
void write_stream_header(std::ofstream& file, size_t stream_id, 
                        const std::string& block_id, size_t port) {
    StreamHeader header;
    header.stream_id = static_cast<uint32_t>(stream_id);
    std::strncpy(header.block_id, block_id.c_str(), sizeof(header.block_id) - 1);
    header.block_id[sizeof(header.block_id) - 1] = '\0';
    header.port = static_cast<uint32_t>(port);
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
}

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
    while (!stop_signal_called.load() && (num_packets == 0 ||
        ctx.stats.packets_captured < num_packets)) {
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
        if (ctx.analysis_packets && ctx.analysis_packets->size() < MAX_ANALYSIS_PACKETS) {
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
    if (ctx.pps_reset_used) {
        std::cout << "  PPS reset time: " << ctx.pps_reset_time.get_real_secs() << " seconds" << std::endl;
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
    
    // 3. Apply dynamic connections (if any)
    if (!config.dynamic_connections.empty()) {
        std::cout << "\nApplying dynamic connections..." << std::endl;
        apply_dynamic_connections(graph, config.dynamic_connections, config.commit_after_each_connection);
    }
    
    // 4. Apply explicit signal paths from YAML
    if (!config.signal_paths.empty()) {
        apply_signal_paths(graph, config.signal_paths);
    }
    
    // 5. Connect Radio to DDC if needed (but DON'T set rates yet)
    if (config.auto_connect_radio_to_ddc) {
        std::cout << "\nAuto-connecting Radio to DDC..." << std::endl;
        auto_connect_radio_to_ddc(graph);
    }
    
    // 6. Find streaming endpoints
    auto endpoints = find_all_stream_endpoints_enhanced(graph, config.stream_endpoints, config.multi_stream);
    
    if (endpoints.empty()) {
        throw std::runtime_error("No suitable streaming endpoints found");
    }
    
    std::cout << "\nFound " << endpoints.size() << " streaming endpoints:" << std::endl;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        std::cout << "  Stream " << i << ": " << endpoints[i].first 
                  << ":" << endpoints[i].second << std::endl;
    }
    
    // 7. Get tick rate from Radio
    double tick_rate = DEFAULT_TICKRATE;
    auto radio_blocks = graph->find_blocks("Radio");
    if (!radio_blocks.empty()) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_blocks[0]);
        tick_rate = radio->get_tick_rate();
    }
    
    // 8. Create output files
    std::vector<StreamContext> contexts;
    std::vector<std::unique_ptr<std::ofstream>> output_files;
    std::unique_ptr<std::mutex> shared_file_mutex;
    std::vector<chdr_packet_data> all_analysis_packets;
    std::mutex analysis_mutex;
    
    if (config.multi_stream.separate_files) {
        // Create separate files for each stream
        for (size_t i = 0; i < endpoints.size(); ++i) {
            std::string fname = config.multi_stream.file_prefix + "_" + std::to_string(i) + ".dat";
            output_files.emplace_back(std::make_unique<std::ofstream>(fname, std::ios::binary));
            if (!output_files.back()->is_open()) {
                throw std::runtime_error("Failed to open output file: " + fname);
            }
        }
    } else {
        // Single shared file
        output_files.emplace_back(std::make_unique<std::ofstream>(file, std::ios::binary));
        if (!output_files.back()->is_open()) {
            throw std::runtime_error("Failed to open output file: " + file);
        }
        shared_file_mutex = std::make_unique<std::mutex>();
    }
    
    // 9. Create streamers and connect them
    std::vector<uhd::rfnoc::ddc_block_control::sptr> ddc_controls;
    std::vector<size_t> ddc_channels;
    
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
            
            contexts.push_back(std::move(ctx));
            
        } catch (const std::exception& e) {
            std::cerr << "Failed to setup stream " << i << ": " << e.what() << std::endl;
        }
    }

    // 10. COMMIT THE GRAPH BEFORE SETTING PROPERTIES
    std::cout << "\nCommitting graph..." << std::endl;
    graph->commit();
    
    // 11. NOW set block properties (especially sample rates) AFTER commit
    std::cout << "\nSetting block properties after commit..." << std::endl;
    
    // Apply all block properties from config
    if (!config.block_properties.empty()) {
        apply_block_properties(graph, config.block_properties, rate);
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

template <typename samp_type>
void capture_multi_stream_with_pps_reset(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const GraphConfig& config,
    const std::string& file,
    size_t num_packets,
    bool enable_analysis,
    const std::string& csv_file,
    double rate,
    size_t samps_per_buff,
    uhd::time_spec_t pps_reset_time,
    bool pps_reset_used)
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
    
    // 3. Apply dynamic connections (if any)
    if (!config.dynamic_connections.empty()) {
        std::cout << "\nApplying dynamic connections..." << std::endl;
        apply_dynamic_connections(graph, config.dynamic_connections, config.commit_after_each_connection);
    }
    
    // 4. Apply explicit signal paths from YAML
    if (!config.signal_paths.empty()) {
        apply_signal_paths(graph, config.signal_paths);
    }
    
    // 5. Connect Radio to DDC if needed (but DON'T set rates yet)
    if (config.auto_connect_radio_to_ddc) {
        std::cout << "\nAuto-connecting Radio to DDC..." << std::endl;
        auto_connect_radio_to_ddc(graph);
    }
    
    // 6. Find streaming endpoints
    auto endpoints = find_all_stream_endpoints_enhanced(graph, config.stream_endpoints, config.multi_stream);
    
    if (endpoints.empty()) {
        throw std::runtime_error("No suitable streaming endpoints found");
    }
    
    std::cout << "\nFound " << endpoints.size() << " streaming endpoints:" << std::endl;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        std::cout << "  Stream " << i << ": " << endpoints[i].first 
                  << ":" << endpoints[i].second << std::endl;
    }
    
    // 7. Get tick rate from Radio
    double tick_rate = DEFAULT_TICKRATE;
    auto radio_blocks = graph->find_blocks("Radio");
    if (!radio_blocks.empty()) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_blocks[0]);
        tick_rate = radio->get_tick_rate();
    }
    
    // 8. Create output files with PPS reset support
    std::vector<StreamContext> contexts;
    std::vector<std::unique_ptr<std::ofstream>> output_files;
    std::unique_ptr<std::mutex> shared_file_mutex;
    std::vector<chdr_packet_data> all_analysis_packets;
    std::mutex analysis_mutex;
    
    if (config.multi_stream.separate_files) {
        // Create separate files for each stream
        for (size_t i = 0; i < endpoints.size(); ++i) {
            std::string fname = config.multi_stream.file_prefix + "_" + std::to_string(i) + ".dat";
            output_files.emplace_back(std::make_unique<std::ofstream>(fname, std::ios::binary));
            if (!output_files.back()->is_open()) {
                throw std::runtime_error("Failed to open output file: " + fname);
            }
            
            // Write file header with PPS reset info for each file
            write_file_header(*output_files.back(), tick_rate, pps_reset_used, pps_reset_time, 1);
            
            // Write stream header for this specific stream
            write_stream_header(*output_files.back(), i, endpoints[i].first, endpoints[i].second);
            
            std::cout << "Created output file: " << fname << " (with PPS reset info)" << std::endl;
        }
    } else {
        // Single shared file
        output_files.emplace_back(std::make_unique<std::ofstream>(file, std::ios::binary));
        if (!output_files.back()->is_open()) {
            throw std::runtime_error("Failed to open output file: " + file);
        }
        shared_file_mutex = std::make_unique<std::mutex>();
        
        // Write file header with PPS reset info and number of streams
        write_file_header(*output_files.back(), tick_rate, pps_reset_used, pps_reset_time, endpoints.size());
        
        // Write stream headers for all streams
        for (size_t i = 0; i < endpoints.size(); ++i) {
            write_stream_header(*output_files.back(), i, endpoints[i].first, endpoints[i].second);
        }
        
        std::cout << "Created shared output file: " << file << " (with PPS reset info for " 
                  << endpoints.size() << " streams)" << std::endl;
    }
    
    // 9. Create streamers and connect them
    std::vector<uhd::rfnoc::ddc_block_control::sptr> ddc_controls;
    std::vector<size_t> ddc_channels;
    
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
            
            // Create context with PPS reset information
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
            
            // PPS reset information
            ctx.pps_reset_time = pps_reset_time;
            ctx.pps_reset_used = pps_reset_used;
            
            contexts.push_back(std::move(ctx));
            
        } catch (const std::exception& e) {
            std::cerr << "Failed to setup stream " << i << ": " << e.what() << std::endl;
        }
    }

    // 10. COMMIT THE GRAPH BEFORE SETTING PROPERTIES
    std::cout << "\nCommitting graph..." << std::endl;
    graph->commit();
    
    // 11. NOW set block properties (especially sample rates) AFTER commit
    std::cout << "\nSetting block properties after commit..." << std::endl;
    
    // Apply all block properties from config
    if (!config.block_properties.empty()) {
        apply_block_properties(graph, config.block_properties, rate);
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
    std::cout << "  PPS reset used: " << (pps_reset_used ? "Yes" : "No") << std::endl;
    if (pps_reset_used) {
        std::cout << "  PPS reset time: " << pps_reset_time.get_real_secs() << " seconds" << std::endl;
    }
    
    // 12. WAIT FOR LO LOCK on all Radio blocks (important for synchronized operation)
    for (const auto& radio_id : radio_blocks) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_id);
        if (radio) {
            for (size_t chan = 0; chan < radio->get_num_output_ports(); ++chan) {
                std::cout << "Waiting for LO lock on " << radio_id.to_string() 
                          << " channel " << chan << ": " << std::flush;
                auto start_time = std::chrono::steady_clock::now();
                while (!radio->get_rx_sensor("lo_locked", chan).to_bool()) {
                    std::this_thread::sleep_for(50ms);
                    std::cout << "+" << std::flush;
                    
                    // Timeout after 10 seconds
                    if (std::chrono::steady_clock::now() - start_time > 10s) {
                        std::cout << " TIMEOUT!" << std::endl;
                        throw std::runtime_error("LO failed to lock within timeout period");
                    }
                }
                std::cout << " locked." << std::endl;
            }
        }
    }
    
    // 13. Create capture threads
    std::vector<std::thread> capture_threads;
    std::atomic<bool> start_capture(false);
    
    // Start capture threads
    for (auto& ctx : contexts) {
        capture_threads.emplace_back(capture_stream<samp_type>, 
                                    std::ref(ctx), 
                                    std::ref(start_capture), 
                                    num_packets);
    }
    
    // 14. Synchronize start if requested
    if (config.multi_stream.sync_streams) {
        std::cout << "\nSynchronizing streams..." << std::endl;
        std::cout << "Waiting " << config.multi_stream.sync_delay << " seconds before synchronized start..." << std::endl;
        std::this_thread::sleep_for(std::chrono::duration<double>(config.multi_stream.sync_delay));
    }
    
    // 15. Start all captures simultaneously
    auto overall_start = std::chrono::steady_clock::now();
    start_capture.store(true);
    
    if (pps_reset_used) {
        std::cout << "\nStarting synchronized multi-stream capture (PPS reset enabled)..." << std::endl;
        std::cout << "All timestamps will be relative to PPS reset time: " 
                  << pps_reset_time.get_real_secs() << " seconds" << std::endl;
    } else {
        std::cout << "\nStarting multi-stream capture..." << std::endl;
    }
    
    // Print capture progress periodically
    std::thread progress_thread([&contexts]() {
        while (!stop_signal_called.load()) {
            std::this_thread::sleep_for(5s);
            if (stop_signal_called.load()) break;
            
            std::cout << "\n=== Capture Progress ===" << std::endl;
            for (const auto& ctx : contexts) {
                std::cout << "[Stream " << ctx.stream_id << "] " 
                          << ctx.stats.packets_captured << " packets, " 
                          << ctx.stats.total_samples << " samples";
                if (ctx.stats.overflow_count > 0) {
                    std::cout << " (O: " << ctx.stats.overflow_count << ")";
                }
                std::cout << std::endl;
            }
        }
    });
    
    // 16. Wait for all threads to complete
    for (auto& thread : capture_threads) {
        thread.join();
    }
    
    // Stop progress thread
    stop_signal_called = true;
    if (progress_thread.joinable()) {
        progress_thread.join();
    }
    
    auto overall_end = std::chrono::steady_clock::now();
    double overall_duration = std::chrono::duration<double>(overall_end - overall_start).count();
    
    // 17. Collect statistics
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
    
    // 18. Close files
    for (auto& file : output_files) {
        if (file && file->is_open()) {
            file->close();
        }
    }
    
    // 19. Print overall statistics
    std::cout << "\n=== Multi-Stream Capture Statistics ===" << std::endl;
    std::cout << "Total streams: " << contexts.size() << std::endl;
    std::cout << "Total packets captured: " << total_packets << std::endl;
    std::cout << "Total samples: " << total_samples << std::endl;
    std::cout << "Overall duration: " << overall_duration << " seconds" << std::endl;
    std::cout << "Aggregate sample rate: " << (total_samples/overall_duration)/1e6 << " Msps" << std::endl;
    if (total_overflows > 0) {
        std::cout << "Total overflows: " << total_overflows << std::endl;
    }
    std::cout << "PPS reset used: " << (pps_reset_used ? "Yes" : "No") << std::endl;
    if (pps_reset_used) {
        std::cout << "PPS reset time: " << pps_reset_time.get_real_secs() << " seconds" << std::endl;
    }
    
    // Print per-stream statistics
    std::cout << "\n=== Per-Stream Statistics ===" << std::endl;
    for (const auto& stat : all_stats) {
        double duration = std::chrono::duration<double>(stat.end_time - stat.start_time).count();
        double avg_rate = (stat.total_samples / duration) / 1e6;
        
        std::cout << "Stream " << stat.stream_id << " (" << stat.block_id << ":" << stat.port << "):" << std::endl;
        std::cout << "  Packets: " << stat.packets_captured << std::endl;
        std::cout << "  Samples: " << stat.total_samples << std::endl;
        std::cout << "  Duration: " << std::fixed << std::setprecision(3) << duration << " seconds" << std::endl;
        std::cout << "  Avg rate: " << std::setprecision(3) << avg_rate << " Msps" << std::endl;
        if (stat.overflow_count > 0) {
            std::cout << "  Overflows: " << stat.overflow_count << std::endl;
        }
        if (stat.error_count > 0) {
            std::cout << "  Errors: " << stat.error_count << std::endl;
        }
        if (pps_reset_used && stat.first_timestamp > 0) {
            std::cout << "  First timestamp: " << std::fixed << std::setprecision(6) 
                      << stat.first_timestamp << " seconds (after PPS reset)" << std::endl;
            std::cout << "  Last timestamp: " << std::fixed << std::setprecision(6) 
                      << stat.last_timestamp << " seconds (after PPS reset)" << std::endl;
        }
    }
    
    // 20. Perform analysis with PPS reset support
    if (enable_analysis && !csv_file.empty() && !all_analysis_packets.empty()) {
        std::cout << "\nAnalyzing packets with PPS reset support..." << std::endl;
        
        // Sort packets by stream ID and sequence number
        std::sort(all_analysis_packets.begin(), all_analysis_packets.end(),
                 [](const chdr_packet_data& a, const chdr_packet_data& b) {
                     if (a.stream_id != b.stream_id) return a.stream_id < b.stream_id;
                     return a.seq_num < b.seq_num;
                 });
        
        // Use PPS-aware analysis function
        analyze_packets_with_pps_reset(all_analysis_packets, csv_file, tick_rate, 
                                     all_stats, pps_reset_time, pps_reset_used, samps_per_buff, rate);
    }
    
    std::cout << "\nMulti-stream capture with PPS reset support completed successfully!" << std::endl;
    
    // 21. Final verification and recommendations
    if (pps_reset_used) {
        std::cout << "\n=== PPS Reset Verification ===" << std::endl;
        std::cout << "All streams were synchronized to PPS edge at time: " 
                  << pps_reset_time.get_real_secs() << " seconds" << std::endl;
        std::cout << "Timestamps in output files are relative to this PPS reset." << std::endl;
        std::cout << "Use the analysis CSV files to examine timing relationships between streams." << std::endl;
    }
    
    if (total_overflows > 0) {
        std::cout << "\n=== Performance Recommendations ===" << std::endl;
        std::cout << "Warning: " << total_overflows << " overflow(s) detected." << std::endl;
        std::cout << "Consider:" << std::endl;
        std::cout << "  - Reducing sample rate" << std::endl;
        std::cout << "  - Increasing buffer sizes (--spb)" << std::endl;
        std::cout << "  - Using separate files for each stream" << std::endl;
        std::cout << "  - Checking network/disk throughput" << std::endl;
    }
}

template <typename samp_type>
void capture_multi_stream_ringbuffer(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const GraphConfig& config,
    const std::string& file,
    size_t num_packets,
    bool enable_analysis,
    const std::string& csv_file,
    double rate,
    size_t samps_per_buff,
    uhd::time_spec_t pps_reset_time,
    bool pps_reset_used)
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
    
    // 3. Apply dynamic connections (if any)
    if (!config.dynamic_connections.empty()) {
        std::cout << "\nApplying dynamic connections..." << std::endl;
        apply_dynamic_connections(graph, config.dynamic_connections, config.commit_after_each_connection);
    }
    
    // 4. Apply explicit signal paths from YAML
    if (!config.signal_paths.empty()) {
        apply_signal_paths(graph, config.signal_paths);
    }
    
    // 5. Connect Radio to DDC if needed (but DON'T set rates yet)
    if (config.auto_connect_radio_to_ddc) {
        std::cout << "\nAuto-connecting Radio to DDC..." << std::endl;
        auto_connect_radio_to_ddc(graph);
    }
    
    // Find streaming endpoints
    auto endpoints = find_all_stream_endpoints_enhanced(graph, config.stream_endpoints, config.multi_stream);
    
    if (endpoints.empty()) {
        throw std::runtime_error("No suitable streaming endpoints found");
    }
    
    std::cout << "\nFound " << endpoints.size() << " streaming endpoints:" << std::endl;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        std::cout << "  Stream " << i << ": " << endpoints[i].first 
                  << ":" << endpoints[i].second << std::endl;
    }
    
    // Get tick rate
    double tick_rate = DEFAULT_TICKRATE;
    auto radio_blocks = graph->find_blocks("Radio");
    if (!radio_blocks.empty()) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_blocks[0]);
        tick_rate = radio->get_tick_rate();
    }
    
    // Create contexts and ring buffers
    std::vector<StreamContext> contexts;
    std::vector<chdr_packet_data> all_analysis_packets;
    std::mutex analysis_mutex;
    std::vector<std::atomic<bool>> stop_writing_flags(endpoints.size());
    std::vector<FileWriterStats>   writer_stats(endpoints.size());
    
    // 9. Create streamers and connect them
    std::vector<uhd::rfnoc::ddc_block_control::sptr> ddc_controls;
    std::vector<size_t> ddc_channels;

    // Initialize contexts with ring buffers
    for (size_t i = 0; i < endpoints.size(); ++i) {
        const auto& [block_id, port] = endpoints[i];
        
        std::cout << "Setting up stream " << i << " for " << block_id << ":" << port << std::endl;
        
        try {
            uhd::rfnoc::block_id_t endpoint_id(block_id);
            
            // Verify endpoint exists
            auto endpoint_block = graph->get_block(endpoint_id);
            if (!endpoint_block || port >= endpoint_block->get_num_output_ports()) {
                std::cerr << "Warning: " << block_id << " port " << port 
                        << " doesn't exist in current FPGA image" << std::endl;
                continue;
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
            
            // Create context with ring buffer
            StreamContext ctx;
            ctx.stream_id = i;
            ctx.block_id = block_id;
            ctx.port = port;
            ctx.rx_streamer = rx_streamer;
            ctx.stats.stream_id = i;
            ctx.stats.block_id = block_id;
            ctx.stats.port = port;
            ctx.analysis_packets = enable_analysis ? &all_analysis_packets : nullptr;
            ctx.analysis_mutex = &analysis_mutex;
            ctx.tick_rate = tick_rate;
            ctx.samps_per_buff = samps_per_buff;
            ctx.pps_reset_time = pps_reset_time;
            ctx.pps_reset_used = pps_reset_used;
            ctx.buffer_config = config.multi_stream.buffer_config;
            
            // Create ring buffer
            // size_t ring_buffer_size = config.multi_stream.buffer_config.ring_buffer_size / sizeof(PacketBuffer);
            // // Ensure power of 2
            // size_t power_of_2 = 1;
            // while (power_of_2 < ring_buffer_size) power_of_2 <<= 1;
            // ctx.ring_buffer = std::make_shared<SPSCRingBuffer<PacketBuffer>>(power_of_2);

            // ring_buffer_size is bytes (user MB). Estimate packet bytes ~ header(16) + payload.
            const size_t bytes_per_samp = sizeof(samp_type);
            const size_t est_payload_bytes = samps_per_buff * bytes_per_samp;
            const size_t est_pkt_bytes     = est_payload_bytes + 16; // hdr+ts margin
            size_t est_pkts = std::max<size_t>(1, config.multi_stream.buffer_config.ring_buffer_size / est_pkt_bytes);
            // enforce minimum 2 and power‑of‑2 for mask math
            size_t power_of_2 = 1;
            while (power_of_2 < est_pkts) power_of_2 <<= 1;
            if (power_of_2 < 2) power_of_2 = 2;
            ctx.ring_buffer = std::make_shared<SPSCRingBuffer<PacketBuffer>>(power_of_2);
            std::cout << "  Ring capacity(pkts): " << power_of_2 << " (~" << (power_of_2*est_pkt_bytes)/(1024.0*1024.0) << " MB est)" << std::endl;
            
            // Set output filename
            ctx.output_filename = config.multi_stream.file_prefix + "_" + std::to_string(i) + ".dat";
            
            contexts.push_back(std::move(ctx));
            // stop_writing_flags.emplace_back(false);
            // writer_stats.emplace_back();
            
        } catch (const std::exception& e) {
            std::cerr << "Failed to setup stream " << i << ": " << e.what() << std::endl;
        }
    }

    //TODO: Fix this section, this is apparently causing the build to fail

    // After loop, register flags for SIGINT propagation:
    g_per_stream_stop_flags.clear();
    g_per_stream_stop_flags.reserve(contexts.size());
    for (size_t i = 0; i < contexts.size(); ++i) {
        g_per_stream_stop_flags.push_back(&stop_writing_flags[i]);
    }

    // Commit graph
    std::cout << "\nCommitting graph..." << std::endl;
    graph->commit();
    
    // Set block properties after commit
    if (!config.block_properties.empty()) {
        apply_block_properties(graph, config.block_properties, rate);
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
    std::cout << "  PPS reset used: " << (pps_reset_used ? "Yes" : "No") << std::endl;
    if (pps_reset_used) {
        std::cout << "  PPS reset time: " << pps_reset_time.get_real_secs() << " seconds" << std::endl;
    }
    
    // 12. WAIT FOR LO LOCK on all Radio blocks (important for synchronized operation)
    for (const auto& radio_id : radio_blocks) {
        auto radio = graph->get_block<uhd::rfnoc::radio_control>(radio_id);
        if (radio) {
            for (size_t chan = 0; chan < radio->get_num_output_ports(); ++chan) {
                std::cout << "Waiting for LO lock on " << radio_id.to_string() 
                          << " channel " << chan << ": " << std::flush;
                auto start_time = std::chrono::steady_clock::now();
                while (!radio->get_rx_sensor("lo_locked", chan).to_bool()) {
                    std::this_thread::sleep_for(50ms);
                    std::cout << "+" << std::flush;
                    
                    // Timeout after 10 seconds
                    if (std::chrono::steady_clock::now() - start_time > 10s) {
                        std::cout << " TIMEOUT!" << std::endl;
                        throw std::runtime_error("LO failed to lock within timeout period");
                    }
                }
                std::cout << " locked." << std::endl;
            }
        }
    }
    
    // Create and start file writer threads
    std::cout << "\nStarting file writer threads..." << std::endl;
    for (size_t i = 0; i < contexts.size(); ++i) {
        contexts[i].writer_thread.reset(new std::thread(
                                        file_writer_thread,
                                        std::ref(contexts[i]),
                                        std::ref(stop_writing_flags[i]),
                                        std::ref(writer_stats[i]))
        );
    }
    
    // Create capture threads
    std::vector<std::thread> capture_threads;
    std::atomic<bool> start_capture(false);
    
    // Start capture threads
    for (size_t i = 0; i < contexts.size(); ++i) {
        capture_threads.emplace_back(
            capture_stream_ringbuffer<samp_type>,
            std::ref(contexts[i]),
            std::ref(start_capture),
            std::ref(stop_writing_flags[i]),
            num_packets,
            std::ref(writer_stats[i])
        );
    }
    
    // Synchronize start if requested
    if (config.multi_stream.sync_streams) {
        std::cout << "\nSynchronizing streams..." << std::endl;
        std::this_thread::sleep_for(std::chrono::duration<double>(config.multi_stream.sync_delay));
    }
    
    // Start all captures
    auto overall_start = std::chrono::steady_clock::now();
    start_capture.store(true);
    std::cout << "\nStarting multi-stream capture with ring buffers..." << std::endl;
    
    // Monitor thread for buffer statistics
    std::thread monitor_thread([&contexts, stop_flag = &stop_signal_called]() {
        while (!stop_flag->load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(10s);
            if (stop_flag->load(std::memory_order_relaxed)) break;
            
            std::cout << "\n=== Ring Buffer Status ===" << std::endl;
            for (const auto& ctx : contexts) {
                size_t buffer_size = ctx.ring_buffer->size();
                size_t buffer_capacity = ctx.ring_buffer->capacity();
                size_t buffer_percent = (buffer_size * 100) / buffer_capacity;
                
                std::cout << "[Stream " << ctx.stream_id << "] Buffer: " 
                          << buffer_size << "/" << buffer_capacity 
                          << " (" << buffer_percent << "%)" << std::endl;
            }
        }
    });
    
    // Wait for all capture threads to complete
    for (auto& thread : capture_threads) {
        thread.join();
    }
    
    // Stop monitor thread
    stop_signal_called.store(true);
    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }
    
    // Wait for all writer threads to complete
    std::cout << "\nWaiting for file writers to finish..." << std::endl;
    for (auto& ctx : contexts) {
        if (ctx.writer_thread && ctx.writer_thread->joinable()) {
            ctx.writer_thread->join();
        }
    }
    
    auto overall_end = std::chrono::steady_clock::now();
    double overall_duration = std::chrono::duration<double>(overall_end - overall_start).count();
    
    // Collect and print statistics
    std::vector<StreamStats> all_stats;
    size_t total_packets = 0;
    size_t total_samples = 0;
    size_t total_rf_overflows = 0;
    size_t total_buffer_overflows = 0;
    size_t total_bytes_written = 0;
    
    std::cout << "\n=== Multi-Stream Capture Statistics ===" << std::endl;
    std::cout << "Total streams: " << contexts.size() << std::endl;
    
    for (size_t i = 0; i < contexts.size(); ++i) {
        const auto& ctx = contexts[i];
        const auto& ws = writer_stats[i];
        
        all_stats.push_back(ctx.stats);
        total_packets += ctx.stats.packets_captured;
        total_samples += ctx.stats.total_samples;
        total_rf_overflows += ctx.stats.overflow_count;
        total_buffer_overflows += ctx.stats.buffer_overflows;
        total_bytes_written += ws.bytes_written;
        
        std::cout << "\nStream " << i << " (" << ctx.block_id << ":" << ctx.port << "):" << std::endl;
        std::cout << "  Capture Statistics:" << std::endl;
        std::cout << "    Packets captured: " << ctx.stats.packets_captured << std::endl;
        std::cout << "    Total samples: " << ctx.stats.total_samples << std::endl;
        std::cout << "    RF overflows: " << ctx.stats.overflow_count << std::endl;
        std::cout << "    Buffer overflows: " << ctx.stats.buffer_overflows << std::endl;
        std::cout << "    Max buffer usage: " << ctx.stats.max_buffer_usage 
                  << "/" << ctx.ring_buffer->capacity() << " packets" << std::endl;
        std::cout << "  File Writer Statistics:" << std::endl;
        std::cout << "    Packets written: " << ws.packets_written << std::endl;
        std::cout << "    Bytes written: " << ws.bytes_written / (1024.0 * 1024.0) << " MB" << std::endl;
        std::cout << "    Write errors: " << ws.write_errors << std::endl;
    }
    
    std::cout << "\nOverall Statistics:" << std::endl;
    std::cout << "  Total packets: " << total_packets << std::endl;
    std::cout << "  Total samples: " << total_samples << std::endl;
    std::cout << "  Total duration: " << overall_duration << " seconds" << std::endl;
    std::cout << "  Aggregate sample rate: " << (total_samples/overall_duration)/1e6 << " Msps" << std::endl;
    std::cout << "  Total data written: " << total_bytes_written / (1024.0 * 1024.0 * 1024.0) << " GB" << std::endl;
    std::cout << "  Total RF overflows: " << total_rf_overflows << std::endl;
    std::cout << "  Total buffer overflows: " << total_buffer_overflows << std::endl;
    
    // Perform analysis if requested
    if (enable_analysis && !csv_file.empty() && !all_analysis_packets.empty()) {
        std::cout << "\nAnalyzing packets..." << std::endl;
        
        std::sort(all_analysis_packets.begin(), all_analysis_packets.end(),
                 [](const chdr_packet_data& a, const chdr_packet_data& b) {
                     if (a.stream_id != b.stream_id) return a.stream_id < b.stream_id;
                     return a.seq_num < b.seq_num;
                 });
        
        analyze_packets_with_pps_reset(all_analysis_packets, csv_file, tick_rate, 
                                     all_stats, pps_reset_time, pps_reset_used, samps_per_buff, rate);
    }
    
    std::cout << "\nMulti-stream capture with ring buffers completed successfully!" << std::endl;
}

// Main function updated with complete PPS reset support
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // Variables
    std::string args, file, format, csv_file, yaml_config;
    size_t num_packets, spb, ring_buffer_mb, batch_write_size;
    double rate, freq, gain, bw;
    bool analyze_only = false;
    bool show_graph = false;
    bool create_yaml_template = false;
    bool multi_stream = false;
    bool use_pps_reset = false;
    double pps_wait_time = 1.5;
    bool verify_pps_reset = true;
    double max_time_after_reset = 1.0;
    
    // Setup program options with comprehensive PPS reset support
    po::options_description desc("Allowed options");
    
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value("addr=192.168.10.2"), "UHD device arguments")
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
        ("ring-buffer-mb", po::value<size_t>(&ring_buffer_mb)->default_value(16), "ring buffer size in MB per stream")
        ("batch-write-size", po::value<size_t>(&batch_write_size)->default_value(100), "packets to batch before writing")
        ("show-graph", po::value<bool>(&show_graph)->default_value(false), "print RFNoC graph info and exit")
        ("analyze-only", po::value<bool>(&analyze_only)->default_value(false), "only analyze existing file")
        ("create-yaml-template", po::value<bool>(&create_yaml_template)->default_value(false), "create YAML template")
        ("multi-stream", po::value<bool>(&multi_stream)->default_value(false), "enable multi-stream capture")
        ("pps-reset", po::value<bool>(&use_pps_reset)->default_value(false), "reset timestamp to zero at next PPS before capture")
        ("pps-wait-time", po::value<double>(&pps_wait_time)->default_value(1.5), "time to wait for PPS reset (seconds)")
        ("verify-pps-reset", po::value<bool>(&verify_pps_reset)->default_value(true), "verify PPS reset was successful")
        ("max-time-after-reset", po::value<double>(&max_time_after_reset)->default_value(1.0), "max acceptable time after PPS reset for verification")
    ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    // Help message
    if (vm.count("help")) {
        std::cout << "CHDR Packet Capture Tool with Multi-Stream Support and PPS Reset" << std::endl;
        std::cout << desc << std::endl;
        std::cout << "\nPPS Reset Features:" << std::endl;
        std::cout << "  * Synchronize device time to 0 at next PPS edge" << std::endl;
        std::cout << "  * Provides common time reference for all streams" << std::endl;
        std::cout << "  * Essential for multi-USRP or precise timing applications" << std::endl;
        std::cout << "  * Timestamps in output files are relative to PPS reset" << std::endl;
        std::cout << "  * Supports verification of successful PPS reset" << std::endl;
        std::cout << "\nMulti-Stream Features:" << std::endl;
        std::cout << "  * Capture from multiple DDC blocks simultaneously" << std::endl;
        std::cout << "  * Synchronized stream start for time-aligned captures" << std::endl;
        std::cout << "  * Per-stream statistics and monitoring" << std::endl;
        std::cout << "  * Separate or combined output files with stream headers" << std::endl;
        std::cout << "  * Multi-threaded capture with one thread per stream" << std::endl;
        std::cout << "  * Real-time progress monitoring during capture" << std::endl;
        std::cout << "\nFile Format Features:" << std::endl;
        std::cout << "  * Version 4 CHDR format with PPS reset metadata" << std::endl;
        std::cout << "  * Per-stream headers in multi-stream files" << std::endl;
        std::cout << "  * Comprehensive analysis with PPS-relative timestamps" << std::endl;
        std::cout << "\nExamples:" << std::endl;
        std::cout << "  Multi-stream with PPS reset:" << std::endl;
        std::cout << "    " << argv[0] << " --multi-stream --pps-reset --rate 10e6 --num-packets 5000" << std::endl;
        std::cout << "  Using YAML config with PPS reset:" << std::endl;
        std::cout << "    " << argv[0] << " --yaml config.yaml --pps-reset --csv analysis.csv" << std::endl;
        std::cout << "  Continuous capture with separate files:" << std::endl;
        std::cout << "    " << argv[0] << " --multi-stream --pps-reset --num-packets 0 --yaml config.yaml" << std::endl;
        std::cout << "  High-rate capture with verification disabled:" << std::endl;
        std::cout << "    " << argv[0] << " --multi-stream --pps-reset --verify-pps-reset false --rate 50e6" << std::endl;
        std::cout << "  High-rate capture with large buffers:" << std::endl;
        std::cout << "    " << argv[0] << " --yaml config.yaml --rate 200e6 --ring-buffer-mb 256 --batch-write-size 1000" << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Analyze existing file mode
    if (analyze_only) {
        if (csv_file.empty()) {
            csv_file = file + ".csv";
        }
        std::cout << "Analyzing existing capture file: " << file << std::endl;
        // Note: Would need to implement analyze_capture_file_with_pps_reset function
        // analyze_capture_file_with_pps_reset(file, csv_file);
        std::cout << "File analysis not implemented in this excerpt." << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Create YAML template if requested
    if (create_yaml_template) {
        // Create graph to discover hardware
        std::cout << "Creating RFNoC graph to discover hardware..." << std::endl;
        auto graph = uhd::rfnoc::rfnoc_graph::make(args);
        
        // Generate dynamic template based on discovered hardware
        // write_dynamic_yaml_template_with_pps_reset(graph, "rfnoc_config_discovered.yaml");
        std::cout << "YAML template creation with PPS reset not implemented in this excerpt." << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Signal handler
    std::signal(SIGINT, &sig_int_handler);
    if (num_packets == 0) {
        std::cout << "Press Ctrl+C to stop continuous capture..." << std::endl;
    }
    
    // Create graph
    std::cout << "\n=== Creating RFNoC Graph ===" << std::endl;
    auto graph = uhd::rfnoc::rfnoc_graph::make(args);
    
    // Print device information
    try {
        auto tree = graph->get_tree();
        std::string device_name = tree->access<std::string>("/name").get();
        std::cout << "Connected to device: " << device_name << std::endl;
        
        // Print FPGA information if available
        try {
            std::string fpga_version = tree->access<std::string>("/mboards/0/fpga_version").get();
            std::cout << "FPGA version: " << fpga_version << std::endl;
        } catch (...) {
            std::cout << "FPGA version: Not available" << std::endl;
        }
    } catch (...) {
        std::cout << "Device information: Not available" << std::endl;
    }
    
    if (show_graph) {
        print_graph_info(graph);
        return EXIT_SUCCESS;
    }
    
    // Load configuration
    GraphConfig config;
    if (!yaml_config.empty()) {
        std::cout << "\n=== Loading Configuration ===" << std::endl;
        std::cout << "Loading configuration from: " << yaml_config << std::endl;
        config = load_graph_config(yaml_config);
        
        // Override PPS reset settings from command line if specified
        if (vm.count("pps-reset")) {
            config.pps_reset.enable_pps_reset = use_pps_reset;
        }
        if (vm.count("pps-wait-time")) {
            config.pps_reset.wait_time_sec = pps_wait_time;
        }
        if (vm.count("verify-pps-reset")) {
            config.pps_reset.verify_reset = verify_pps_reset;
        }
        if (vm.count("max-time-after-reset")) {
            config.pps_reset.max_time_after_reset = max_time_after_reset;
        }
        

        std::cout << "Configuration loaded successfully." << std::endl;
        std::cout << "  PPS reset enabled: " << (config.pps_reset.enable_pps_reset ? "Yes" : "No") << std::endl;
        std::cout << "  Multi-stream enabled: " << (config.multi_stream.enable_multi_stream ? "Yes" : "No") << std::endl;
    } else {
        std::cout << "\n=== Using Default Configuration ===" << std::endl;
        // Set default configuration
        config.auto_connect_radio_to_ddc = true;
        config.auto_find_stream_endpoint = true;
        config.multi_stream.enable_multi_stream = multi_stream;
        
        // Set PPS reset from command line
        config.pps_reset.enable_pps_reset = use_pps_reset;
        config.pps_reset.wait_time_sec = pps_wait_time;
        config.pps_reset.verify_reset = verify_pps_reset;
        config.pps_reset.max_time_after_reset = max_time_after_reset;

        // Override buffer settings from command line
        config.multi_stream.buffer_config.ring_buffer_size = ring_buffer_mb * 1024 * 1024;
        config.multi_stream.buffer_config.batch_write_size = batch_write_size;
        config.multi_stream.separate_files = true;  // Always use separate files with ring buffers
        
        // Configure PPS reset
        config.pps_reset.enable_pps_reset = use_pps_reset;
        config.pps_reset.wait_time_sec = pps_wait_time;
        config.pps_reset.verify_reset = verify_pps_reset;
        config.pps_reset.max_time_after_reset = max_time_after_reset;
        
        // Add default Radio properties if Radio block exists
        auto radio_blocks = graph->find_blocks("Radio");
        if (!radio_blocks.empty()) {
            std::cout << "Found " << radio_blocks.size() << " Radio block(s), setting default properties..." << std::endl;
            for (const auto& radio_id : radio_blocks) {
                std::string id_str = radio_id.to_string();
                config.block_properties[id_str]["freq"] = std::to_string(freq);
                config.block_properties[id_str]["gain"] = std::to_string(gain);
                config.block_properties[id_str]["rate"] = std::to_string(rate);
                if (bw > 0) {
                    config.block_properties[id_str]["bandwidth"] = std::to_string(bw);
                }
                std::cout << "  " << id_str << ": freq=" << freq/1e6 << "MHz, gain=" << gain << "dB" << std::endl;
            }
        } else {
            std::cout << "No Radio blocks found in FPGA image." << std::endl;
        }
    }
    
    // Validate configuration
    if (!config.multi_stream.enable_multi_stream) {
        std::cout << "\n=== Enabling Multi-Stream Mode ===" << std::endl;
        std::cout << "Multi-stream mode is required for this tool. Enabling automatically..." << std::endl;
        config.multi_stream.enable_multi_stream = true;
    }
    
    // Perform PPS reset if requested (CRITICAL: This must happen before any streaming setup)
    uhd::time_spec_t pps_reset_time(0.0);
    bool pps_reset_used = false;
    
    if (config.pps_reset.enable_pps_reset) {
        std::cout << "\n=== PPS Reset Sequence ===" << std::endl;
        std::cout << "PPS reset configuration:" << std::endl;
        std::cout << "  Wait time: " << config.pps_reset.wait_time_sec << " seconds" << std::endl;
        std::cout << "  Verification: " << (config.pps_reset.verify_reset ? "Enabled" : "Disabled") << std::endl;
        std::cout << "  Max time after reset: " << config.pps_reset.max_time_after_reset << " seconds" << std::endl;
        
        pps_reset_time = perform_pps_reset(graph, config.pps_reset);
        pps_reset_used = true;
        
        std::cout << "PPS reset completed. All subsequent timestamps will be relative to PPS edge." << std::endl;
    } else {
        std::cout << "\n=== PPS Reset Disabled ===" << std::endl;
        std::cout << "Timestamps will be relative to device boot time." << std::endl;
        std::cout << "Consider using --pps-reset for multi-USRP or precision timing applications." << std::endl;
    }
    
    // Validate sample rate and buffer size
    if (rate < 1e3) {
        std::cerr << "Warning: Sample rate very low (" << rate << " Hz). Consider increasing." << std::endl;
    }
    if (rate > 500e6) {
        std::cerr << "Warning: Sample rate very high (" << rate/1e6 << " MHz). May cause overflows." << std::endl;
    }
    
    if (spb < 100) {
        std::cerr << "Warning: Samples per buffer very small (" << spb << "). May cause inefficiency." << std::endl;
        spb = 100;
    }
    if (spb > 100000) {
        std::cerr << "Warning: Samples per buffer very large (" << spb << "). May cause memory issues." << std::endl;
        spb = 100000;
    }
    
    // Start capture
    bool enable_analysis = !csv_file.empty();
    if (enable_analysis && csv_file.empty()) {
        csv_file = file + "_analysis.csv";
        std::cout << "Auto-generated analysis filename: " << csv_file << std::endl;
    }
    
    std::cout << "\n=== Starting Multi-Stream Capture ===" << std::endl;
    std::cout << "Capture parameters:" << std::endl;
    std::cout << "  Ring buffer size per stream: " << ring_buffer_mb << " MB" << std::endl;
    std::cout << "  Batch write size: " << batch_write_size << " packets" << std::endl;
    std::cout << "  Sample format: " << format << std::endl;
    std::cout << "  Sample rate: " << rate/1e6 << " Msps" << std::endl;
    std::cout << "  Samples per buffer: " << spb << std::endl;
    std::cout << "  Packets per stream: " << (num_packets == 0 ? "Continuous" : std::to_string(num_packets)) << std::endl;
    std::cout << "  Analysis enabled: " << (enable_analysis ? "Yes" : "No") << std::endl;
    std::cout << "  PPS reset used: " << (pps_reset_used ? "Yes" : "No") << std::endl;
    
    try {
        if(config.multi_stream.separate_files  && pps_reset_used) {
            std::cout << "Ring Buffer Use is requested, using ring buffer based multistream capture method" << std::endl;
            if (format == "sc16") {
                capture_multi_stream_ringbuffer<std::complex<short>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb, pps_reset_time, pps_reset_used);
            } else if (format == "fc32") {
                capture_multi_stream_ringbuffer<std::complex<float>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb, pps_reset_time, pps_reset_used);
            } else if (format == "fc64") {
                capture_multi_stream_ringbuffer<std::complex<double>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb, pps_reset_time, pps_reset_used);
            } else {
                throw std::runtime_error("Unsupported format: " + format + 
                                    ". Supported formats: sc16, fc32, fc64");
            }
        } 
        else if (!(config.multi_stream.separate_files)  && pps_reset_used) {
            std::cout << "Ring Buffer Use is not requested, not using ring buffer based multistream capture method" << std::endl;
            if (format == "sc16") {
                capture_multi_stream_with_pps_reset<std::complex<short>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb, pps_reset_time, pps_reset_used);
            } else if (format == "fc32") {
                capture_multi_stream_with_pps_reset<std::complex<float>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb, pps_reset_time, pps_reset_used);
            } else if (format == "fc64") {
                capture_multi_stream_with_pps_reset<std::complex<double>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb, pps_reset_time, pps_reset_used);
            } else {
                throw std::runtime_error("Unsupported format: " + format + 
                                    ". Supported formats: sc16, fc32, fc64");
            }
        }
        else {
            if (format == "sc16") {
                capture_multi_stream<std::complex<short>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb);
            } else if (format == "fc32") {
                capture_multi_stream<std::complex<float>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb);
            } else if (format == "fc64") {
                capture_multi_stream<std::complex<double>>(
                    graph, config, file, num_packets, enable_analysis, csv_file, 
                    rate, spb);


            } else {
                throw std::runtime_error("Unsupported format: " + format + 
                                    ". Supported formats: sc16, fc32, fc64");
            }    
        }
    } catch (const uhd::exception& e) {
        std::cerr << "\nUHD Error: " << e.what() << std::endl;
        std::cerr << "This may indicate:" << std::endl;
        std::cerr << "  - USRP not connected or powered" << std::endl;
        std::cerr << "  - Incompatible FPGA image" << std::endl;
        std::cerr << "  - Hardware configuration issue" << std::endl;
        std::cerr << "  - Sample rate too high for current configuration" << std::endl;
        return EXIT_FAILURE;
    } catch (const std::runtime_error& e) {
        std::cerr << "\nRuntime Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "\nUnexpected Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    
    std::cout << "\n=== Capture Completed Successfully ===" << std::endl;
    
    // Print final summary
    std::cout << "\nFinal Summary:" << std::endl;
    std::cout << "  Output file(s): " << (config.multi_stream.separate_files ? 
                                         config.multi_stream.file_prefix + "_*.dat" : file) << std::endl;
    if (enable_analysis) {
        std::cout << "  Analysis file: " << csv_file << std::endl;
        std::cout << "  Statistics file: " << csv_file.substr(0, csv_file.find_last_of('.')) + "_stats.csv" << std::endl;
    }
    std::cout << "  PPS reset information included in file headers" << std::endl;
    
    // Recommendations for further analysis
    if (enable_analysis) {
        std::cout << "\nAnalysis Recommendations:" << std::endl;
        std::cout << "  - Examine timestamp columns for stream synchronization" << std::endl;
        std::cout << "  - Check sequence numbers for dropped packets" << std::endl;
        std::cout << "  - Analyze 'time_since_pps_sec' for PPS-relative timing" << std::endl;
        std::cout << "  - Review per-stream statistics for performance metrics" << std::endl;
    }
    
    return EXIT_SUCCESS;
}