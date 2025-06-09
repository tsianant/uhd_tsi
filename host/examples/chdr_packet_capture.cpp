//
// Copyright 2024 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CHDR Packet Capture and Analysis Utility
//
// This utility captures sample data from RFNoC blocks and stores it with reconstructed 
// CHDR (Condensed Hierarchical Datagram for RFNoC) formatting. The CHDR headers are 
// reconstructed based on stream metadata rather than being raw transport layer packets.
//
// Key Features:
// - Automatically handles RFNoC static connections (e.g., Radio->DDC->SEP chains)
// - Visualizes RFNoC graph structure before capture
// - Stores data in binary format with CHDR headers for analysis
// - Provides CSV export for packet field analysis
//
// Note: For applications requiring true raw CHDR packet capture at the transport layer,
// modifications to UHD's transport layer would be necessary. This utility operates at
// the streamer API level, which provides sample data with metadata.
//

#include <uhd/exception.hpp>
#include <uhd/rfnoc_graph.hpp>
#include <uhd/rfnoc/noc_block_base.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/rfnoc/ddc_block_control.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/time_spec.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/utils/graph_utils.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
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

namespace po = boost::program_options;
using namespace std::chrono_literals;

// Global flag for Ctrl+C handling
static volatile bool stop_signal_called = false;

void sig_int_handler(int)
{
    stop_signal_called = true;
}

// Structure to hold CHDR packet data
struct chdr_packet_data {
    uint64_t header;
    uint64_t timestamp;
    std::vector<uint64_t> metadata;
    std::vector<uint8_t> payload;
};

// Parse CHDR header fields based on CHDR width
struct chdr_header_fields {
    uint8_t vc;           // Virtual Channel (6 bits)
    bool eob;             // End of Burst (1 bit)
    bool eov;             // End of Vector (1 bit)
    uint8_t pkt_type;     // Packet Type (3 bits)
    uint8_t num_mdata;    // Number of metadata words (5 bits)
    uint16_t seq_num;     // Sequence Number (16 bits)
    uint16_t length;      // Packet length in bytes (16 bits)
    uint16_t dst_epid;    // Destination Endpoint ID (16 bits)
    
    void parse(uint64_t header) {
        dst_epid = header & 0xFFFF;
        length = (header >> 16) & 0xFFFF;
        seq_num = (header >> 32) & 0xFFFF;
        num_mdata = (header >> 48) & 0x1F;
        pkt_type = (header >> 53) & 0x7;
        eov = (header >> 56) & 0x1;
        eob = (header >> 57) & 0x1;
        vc = (header >> 58) & 0x3F;
    }
    
    std::string pkt_type_str() const {
        switch(pkt_type) {
            case 0x0: return "Management";
            case 0x1: return "Stream Status";
            case 0x2: return "Stream Command";
            case 0x4: return "Control Transaction";
            case 0x6: return "Data (No TS)";
            case 0x7: return "Data (With TS)";
            default: return "Reserved";
        }
    }
};

// Forward declaration
void analyze_packets(const std::vector<chdr_packet_data>& packets, 
                    const std::string& csv_file,
                    size_t chdr_width_bits);

// Print RFNoC graph information
void print_graph_info(uhd::rfnoc::rfnoc_graph::sptr graph)
{
    std::cout << "\n=== RFNoC Graph Information ===" << std::endl;
    
    // Print all blocks
    std::cout << "\nRFNoC blocks on this device:" << std::endl;
    auto block_ids = graph->find_blocks("");
    for (const auto& block_id : block_ids) {
        std::cout << "  * " << block_id.to_string() << std::endl;
    }
    
    // Print static connections
    std::cout << "\nStatic connections:" << std::endl;
    auto static_edges = graph->enumerate_static_connections();
    for (const auto& edge : static_edges) {
        std::cout << "  * " << edge.to_string() << std::endl;
    }
    
    std::cout << std::endl;
}

// Find the last block in the RX chain that connects to an SEP
uhd::rfnoc::graph_edge_t find_rx_edge_to_sep(
    uhd::rfnoc::rfnoc_graph::sptr graph,
    const uhd::rfnoc::block_id_t& start_block,
    size_t start_port)
{
    std::cout << "Finding path from " << start_block << ":" << start_port 
              << " to SEP..." << std::endl;
    
    // Get the block chain starting from the radio
    auto edges = uhd::rfnoc::get_block_chain(
        graph, start_block, start_port, true /* source chain */);
    
    if (edges.empty()) {
        throw uhd::runtime_error("No path found from " + start_block.to_string() + " to SEP!");
    }
    
    // Print the path
    std::cout << "Path found:" << std::endl;
    for (const auto& edge : edges) {
        std::cout << "  " << edge.to_string() << std::endl;
    }
    
    // The last edge should connect to a SEP

    

    auto last_edge = edges.back();
    std::cout << "Will connect to: " << last_edge.src_blockid 
              << ":" << last_edge.src_port << std::endl;
    
    return last_edge;
}

// Fixed capture_chdr_packets function with proper SPP/SPB configuration
template <typename samp_type>
void capture_chdr_packets(
    uhd::rx_streamer::sptr rx_stream,
    const std::string& file,
    size_t num_packets,
    double time_requested,
    bool enable_analysis,
    const std::string& csv_file,
    size_t chdr_width_bits,
    double rate,
    size_t samps_per_buff)
{
    // Open binary file for writing
    std::ofstream outfile(file.c_str(), std::ios::binary);
    if (!outfile.is_open()) {
        throw std::runtime_error("Failed to open file: " + file);
    }
    
    // Write header with CHDR width info
    uint32_t file_header[2];
    file_header[0] = 0x43484452; // "CHDR" magic number
    file_header[1] = chdr_width_bits;
    outfile.write(reinterpret_cast<const char*>(file_header), sizeof(file_header));
    
    // Get actual streaming parameters from the streamer
    const size_t max_samps_per_packet = rx_stream->get_max_num_samps();
    const size_t chdr_width_bytes = chdr_width_bits / 8;
    
    // Use the smaller of requested SPB or max samples per packet
    size_t effective_spb = std::min(samps_per_buff, max_samps_per_packet);
    
    std::cout << "Stream configuration:" << std::endl;
    std::cout << "  Max samples per packet: " << max_samps_per_packet << std::endl;
    std::cout << "  Requested samples per buffer: " << samps_per_buff << std::endl;  
    std::cout << "  Effective samples per buffer: " << effective_spb << std::endl;
    std::cout << "  Sample size: " << sizeof(samp_type) << " bytes" << std::endl;
    std::cout << "  CHDR width: " << chdr_width_bits << " bits" << std::endl;
    
    // Buffer for raw packet data
    std::vector<uint8_t> packet_buffer;
    packet_buffer.reserve(max_samps_per_packet * sizeof(samp_type) + 64); // Extra space for headers
    
    // Metadata
    uhd::rx_metadata_t md;
    
    // Setup streaming with proper sample counts
    uhd::stream_cmd_t stream_cmd(num_packets == 0 ? 
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS :
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    
    // If capturing specific number of packets, calculate total samples needed
    if (num_packets > 0) {
        stream_cmd.num_samps = num_packets * effective_spb;
        std::cout << "Requesting " << stream_cmd.num_samps << " total samples (" 
                  << num_packets << " packets * " << effective_spb << " samples/packet)" << std::endl;
    }
    
    stream_cmd.stream_now = (time_requested == 0.0);
    if (!stream_cmd.stream_now) {
        stream_cmd.time_spec = uhd::time_spec_t(time_requested);
        std::cout << "Scheduled start time: " << time_requested << " seconds" << std::endl;
    }
    
    rx_stream->issue_stream_cmd(stream_cmd);
    
    // Packet capture statistics
    size_t packets_captured = 0;
    size_t total_samples_received = 0;
    size_t total_bytes = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    std::cout << "\nStarting CHDR packet capture..." << std::endl;
    
    // For analysis
    std::vector<chdr_packet_data> captured_packets;
    
    // Main capture loop
    while (!stop_signal_called && (num_packets == 0 || packets_captured < num_packets)) {
        // Create buffer vector for recv - use effective_spb size
        std::vector<samp_type> buff(effective_spb);
        std::vector<void*> buff_ptrs;
        buff_ptrs.push_back(&buff.front());
        
        // Receive samples with reasonable timeout
        size_t num_rx_samps = rx_stream->recv(
            buff_ptrs, effective_spb, md, 3.0, false);
        
        // Handle errors
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << "\nTimeout while streaming (received " << packets_captured 
                      << " packets so far)" << std::endl;
            if (packets_captured == 0) {
                std::cerr << "No packets received - check configuration!" << std::endl;
                break;
            }
            // Continue for now, might get more packets
            continue;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            std::cerr << "O" << std::flush; // Brief overflow indicator
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "\nReceiver error: " << md.strerror() << std::endl;
            break;
        }
        
        // Only process if we actually received samples
        if (num_rx_samps == 0) {
            continue;
        }
        
        // Calculate packet sizes
        size_t sample_size_bytes = num_rx_samps * sizeof(samp_type);
        size_t timestamp_size = md.has_time_spec ? 8 : 0;
        size_t total_packet_size = 8 + timestamp_size + sample_size_bytes; // header + optional timestamp + data
        
        // Construct CHDR packet
        packet_buffer.clear();
        packet_buffer.reserve(total_packet_size);
        
        // Create CHDR header
        uint64_t header = 0;
        header |= (uint64_t)(0x0000) & 0xFFFF;                       // DstEPID (0 for now)
        header |= ((uint64_t)total_packet_size & 0xFFFF) << 16;      // Length
        header |= ((uint64_t)(packets_captured & 0xFFFF)) << 32;     // SeqNum
        header |= ((uint64_t)0 & 0x1F) << 48;                        // NumMData (0 for data packets)
        header |= ((uint64_t)(md.has_time_spec ? 0x7 : 0x6) & 0x7) << 53; // PktType (0x7=data with TS, 0x6=data no TS)
        header |= ((uint64_t)(md.end_of_burst ? 1 : 0) & 0x1) << 57; // EOB
        header |= ((uint64_t)0 & 0x3F) << 58;                        // VC (Virtual Channel)
        
        // Add header to packet buffer
        const uint8_t* header_bytes = reinterpret_cast<const uint8_t*>(&header);
        packet_buffer.insert(packet_buffer.end(), header_bytes, header_bytes + 8);
        
        // Add timestamp if present
        if (md.has_time_spec) {
            uint64_t timestamp = md.time_spec.to_ticks(rate);
            const uint8_t* ts_bytes = reinterpret_cast<const uint8_t*>(&timestamp);
            packet_buffer.insert(packet_buffer.end(), ts_bytes, ts_bytes + 8);
        }
        
        // Add sample data
        const uint8_t* sample_bytes = reinterpret_cast<const uint8_t*>(buff.data());
        packet_buffer.insert(packet_buffer.end(), sample_bytes, sample_bytes + sample_size_bytes);
        
        // Write packet to file: [size][packet_data]
        uint32_t pkt_size = static_cast<uint32_t>(packet_buffer.size());
        outfile.write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
        outfile.write(reinterpret_cast<const char*>(packet_buffer.data()), packet_buffer.size());
        
        // Store for analysis if requested
        if (enable_analysis && captured_packets.size() < 10000) { // Limit memory usage
            chdr_packet_data pkt;
            
            // Parse header
            const uint64_t* data_ptr = reinterpret_cast<const uint64_t*>(packet_buffer.data());
            pkt.header = data_ptr[0];
            
            // Extract timestamp if present
            if (md.has_time_spec) {
                pkt.timestamp = data_ptr[1];
            }
            
            // Store payload (sample data only)
            size_t header_offset = 8 + timestamp_size;
            pkt.payload.assign(packet_buffer.begin() + header_offset, packet_buffer.end());
            
            captured_packets.push_back(pkt);
        }
        
        // Update statistics
        packets_captured++;
        total_samples_received += num_rx_samps;
        total_bytes += packet_buffer.size();
        
        // Progress update every 100 packets (not too frequent)
        if (packets_captured % 100 == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            double elapsed_secs = std::chrono::duration<double>(elapsed).count();
            double sample_rate = total_samples_received / elapsed_secs;
            double data_rate = total_bytes / elapsed_secs;
            
            std::cout << "\rPackets: " << packets_captured 
                      << ", Samples: " << total_samples_received 
                      << ", Rate: " << sample_rate/1e6 << " Msps"
                      << ", Data: " << data_rate/1024.0/1024.0 << " MB/s" << std::flush;
        }
    }
    
    std::cout << std::endl;
    outfile.close();
    
    // Stop streaming
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(stream_cmd);
    
    // Final statistics
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    double elapsed_secs = std::chrono::duration<double>(elapsed).count();
    
    std::cout << "\n=== Capture Statistics ===" << std::endl;
    std::cout << "  Total packets captured: " << packets_captured << std::endl;
    std::cout << "  Total samples received: " << total_samples_received << std::endl;
    std::cout << "  Total data captured: " << total_bytes/1024.0/1024.0 << " MB" << std::endl;
    std::cout << "  Capture duration: " << elapsed_secs << " seconds" << std::endl;
    std::cout << "  Average sample rate: " << (total_samples_received/elapsed_secs)/1e6 << " Msps" << std::endl;
    std::cout << "  Average data rate: " << (total_bytes/elapsed_secs)/1024.0/1024.0 << " MB/s" << std::endl;
    std::cout << "  Average samples per packet: " << (packets_captured > 0 ? total_samples_received/packets_captured : 0) << std::endl;
    
    // Perform analysis if requested
    if (enable_analysis && !csv_file.empty() && !captured_packets.empty()) {
        std::cout << "\nAnalyzing " << captured_packets.size() << " captured packets..." << std::endl;
        analyze_packets(captured_packets, csv_file, chdr_width_bits);
    }
}

// Enhanced analysis function with better sample handling
void analyze_packets(const std::vector<chdr_packet_data>& packets, 
                    const std::string& csv_file,
                    size_t chdr_width_bits)
{
    std::ofstream csv(csv_file);
    if (!csv.is_open()) {
        throw std::runtime_error("Failed to open CSV file: " + csv_file);
    }
    
    // Write CSV header
    csv << "packet_num,vc,eob,eov,pkt_type,pkt_type_str,num_mdata,seq_num,length,"
        << "dst_epid,has_timestamp,timestamp,payload_size,sample_format,num_samples,"
        << "first_sample_hex,first_sample_real,first_sample_imag" << std::endl;
    
    // Analyze each packet
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& pkt = packets[i];
        chdr_header_fields hdr;
        hdr.parse(pkt.header);
        
        // Write packet info to CSV
        csv << i << ","
            << std::hex << std::setfill('0')
            << "0x" << std::setw(2) << static_cast<int>(hdr.vc) << ","
            << (hdr.eob ? "1" : "0") << ","
            << (hdr.eov ? "1" : "0") << ","
            << "0x" << static_cast<int>(hdr.pkt_type) << ","
            << hdr.pkt_type_str() << ","
            << std::dec << static_cast<int>(hdr.num_mdata) << ","
            << hdr.seq_num << ","
            << hdr.length << ","
            << std::hex << "0x" << std::setw(4) << hdr.dst_epid << ",";
        
        // Timestamp
        bool has_ts = (hdr.pkt_type == 0x7);
        csv << (has_ts ? "1" : "0") << ",";
        if (has_ts) {
            csv << "0x" << std::setw(16) << pkt.timestamp;
        } else {
            csv << "N/A";
        }
        
        // Payload info
        csv << "," << std::dec << pkt.payload.size() << ",";
        
        // Determine sample format and parse first sample
        if (pkt.payload.size() >= 4) {  // At least one sc16 sample
            // Assume sc16 format (most common)
            size_t samples_per_payload = pkt.payload.size() / 4;  // 4 bytes per sc16 sample
            csv << "sc16," << samples_per_payload << ",";
            
            // Extract first sample as sc16 (2 bytes real, 2 bytes imag)
            if (pkt.payload.size() >= 4) {
                const int16_t* sample_data = reinterpret_cast<const int16_t*>(pkt.payload.data());
                int16_t real_part = sample_data[0];
                int16_t imag_part = sample_data[1];
                
                // Hex representation
                csv << "0x";
                for (int j = 0; j < 4; ++j) {
                    csv << std::hex << std::setw(2) << std::setfill('0') 
                        << static_cast<unsigned int>(pkt.payload[j]);
                }
                csv << "," << std::dec << real_part << "," << imag_part;
            } else {
                csv << "N/A,N/A,N/A";
            }
        } else if (pkt.payload.size() >= 8) {  // Could be fc32
            size_t samples_per_payload = pkt.payload.size() / 8;  // 8 bytes per fc32 sample
            csv << "fc32," << samples_per_payload << ",";
            
            const float* sample_data = reinterpret_cast<const float*>(pkt.payload.data());
            float real_part = sample_data[0];
            float imag_part = sample_data[1];
            
            // Hex representation of first 8 bytes
            csv << "0x";
            for (int j = 0; j < std::min(8, (int)pkt.payload.size()); ++j) {
                csv << std::hex << std::setw(2) << std::setfill('0') 
                    << static_cast<unsigned int>(pkt.payload[j]);
            }
            csv << "," << std::dec << real_part << "," << imag_part;
        } else {
            csv << "unknown,0,";
            // Just show first few bytes as hex
            if (pkt.payload.size() > 0) {
                csv << "0x";
                for (size_t j = 0; j < std::min((size_t)8, pkt.payload.size()); ++j) {
                    csv << std::hex << std::setw(2) << std::setfill('0') 
                        << static_cast<unsigned int>(pkt.payload[j]);
                }
                csv << ",N/A,N/A";
            } else {
                csv << "N/A,N/A,N/A";
            }
        }
        
        csv << std::endl;
    }
    
    csv.close();
    std::cout << "Analysis complete. Results written to: " << csv_file << std::endl;
}

// Helper function to format sample data for different types
template<typename T>
std::string format_sample_data(const std::vector<uint8_t>& payload, size_t max_samples = 5) {
    if (payload.size() < sizeof(std::complex<T>)) {
        return "N/A";
    }
    
    std::stringstream ss;
    const std::complex<T>* samples = reinterpret_cast<const std::complex<T>*>(payload.data());
    size_t num_samples = std::min(max_samples, payload.size() / sizeof(std::complex<T>));
    
    ss << "[";
    for (size_t i = 0; i < num_samples; ++i) {
        if (i > 0) ss << ", ";
        ss << "(" << samples[i].real() << "+" << samples[i].imag() << "j)";
    }
    if (num_samples < payload.size() / sizeof(std::complex<T>)) {
        ss << "...";
    }
    ss << "]";
    
    return ss.str();
}

// Function to dump raw payload data as hex for debugging
void dump_payload_hex(const std::vector<uint8_t>& payload, const std::string& filename) {
    std::ofstream hexfile(filename);
    if (!hexfile.is_open()) {
        throw std::runtime_error("Failed to open hex dump file: " + filename);
    }
    
    hexfile << "Payload size: " << payload.size() << " bytes\n";
    hexfile << "Hex dump:\n";
    
    for (size_t i = 0; i < payload.size(); i += 16) {
        // Address
        hexfile << std::hex << std::setw(8) << std::setfill('0') << i << ": ";
        
        // Hex bytes
        for (size_t j = 0; j < 16 && (i + j) < payload.size(); ++j) {
            hexfile << std::hex << std::setw(2) << std::setfill('0') 
                   << static_cast<unsigned int>(payload[i + j]) << " ";
        }
        
        // ASCII representation
        hexfile << " |";
        for (size_t j = 0; j < 16 && (i + j) < payload.size(); ++j) {
            uint8_t byte = payload[i + j];
            hexfile << (isprint(byte) ? static_cast<char>(byte) : '.');
        }
        hexfile << "|\n";
    }
    
    hexfile.close();
}
// Analyze existing capture file
void analyze_capture_file(const std::string& input_file, 
                         const std::string& csv_file)
{
    std::ifstream infile(input_file, std::ios::binary);
    if (!infile.is_open()) {
        throw std::runtime_error("Failed to open input file: " + input_file);
    }
    
    // Read file header
    uint32_t file_header[2];
    infile.read(reinterpret_cast<char*>(file_header), sizeof(file_header));
    
    if (file_header[0] != 0x43484452) {
        throw std::runtime_error("Invalid file format - not a CHDR capture file");
    }
    
    size_t chdr_width_bits = file_header[1];
    std::cout << "CHDR capture file analysis" << std::endl;
    std::cout << "CHDR Width: " << chdr_width_bits << " bits" << std::endl;
    
    std::vector<chdr_packet_data> packets;
    
    // Read packets
    while (!infile.eof()) {
        uint32_t pkt_size;
        infile.read(reinterpret_cast<char*>(&pkt_size), sizeof(pkt_size));
        
        if (infile.eof()) break;
        
        std::vector<uint8_t> packet_buffer(pkt_size);
        infile.read(reinterpret_cast<char*>(packet_buffer.data()), pkt_size);
        
        if (infile.gcount() != pkt_size) {
            std::cerr << "Warning: Incomplete packet at end of file" << std::endl;
            break;
        }
        
        // Parse packet
        chdr_packet_data pkt;
        const uint64_t* data_ptr = reinterpret_cast<const uint64_t*>(packet_buffer.data());
        pkt.header = data_ptr[0];
        
        chdr_header_fields hdr;
        hdr.parse(pkt.header);
        
        // Extract timestamp if present
        if (hdr.pkt_type == 0x7) {
            if (chdr_width_bits == 64) {
                pkt.timestamp = data_ptr[1];
            } else {
                pkt.timestamp = data_ptr[1]; // For wider CHDR, timestamp is in second word
            }
        }
        
        // Store payload
        size_t header_offset = chdr_width_bits / 8;
        if (chdr_width_bits == 64 && hdr.pkt_type == 0x7) {
            header_offset = 16; // Header + timestamp
        }
        
        pkt.payload.assign(packet_buffer.begin() + header_offset, packet_buffer.end());
        packets.push_back(pkt);
    }
    
    infile.close();
    
    std::cout << "Read " << packets.size() << " packets from file" << std::endl;
    
    // Analyze packets
    analyze_packets(packets, csv_file, chdr_width_bits);
}

// Updated main function with better SPP/SPB defaults and configuration
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // Variables for program options
    std::string args, file, format, csv_file, block_id;
    size_t radio_id, radio_chan, block_port;
    size_t num_packets, chdr_width, spp, spb;
    double rate, freq, gain, bw, time_requested;
    bool analyze_only = false;
    bool show_graph = false;
    
    // Setup program options with better defaults
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "UHD device arguments")
        ("file", po::value<std::string>(&file)->default_value("chdr_capture.dat"), "output filename")
        ("analyze-only", po::value<bool>(&analyze_only)->default_value(false), "only analyze existing file")
        ("csv", po::value<std::string>(&csv_file)->default_value(""), "CSV output file for analysis")
        ("num-packets", po::value<size_t>(&num_packets)->default_value(0), "number of packets to capture (0 for continuous)")
        ("rate", po::value<double>(&rate)->default_value(1e6), "sample rate")
        ("freq", po::value<double>(&freq)->default_value(0.0), "center frequency")
        ("gain", po::value<double>(&gain)->default_value(0.0), "gain")
        ("bw", po::value<double>(&bw)->default_value(0.0), "analog bandwidth")
        ("radio-id", po::value<size_t>(&radio_id)->default_value(0), "radio block to use")
        ("radio-chan", po::value<size_t>(&radio_chan)->default_value(0), "radio channel to use")
        ("block-id", po::value<std::string>(&block_id), "optional block to insert in chain")
        ("block-port", po::value<size_t>(&block_port)->default_value(0), "port on optional block")
        ("format", po::value<std::string>(&format)->default_value("sc16"), "sample format (sc16, fc32, fc64)")
        ("chdr-width", po::value<size_t>(&chdr_width)->default_value(64), "CHDR width in bits")
        ("time", po::value<double>(&time_requested)->default_value(0.0), "time to start capture")
        ("show-graph", po::value<bool>(&show_graph)->default_value(false), "print RFNoC graph info")
        ("spp", po::value<size_t>(&spp), "samples per packet (CRITICAL: min 200 for 1GigE, min 1000 for 10GigE)")
        ("spb", po::value<size_t>(&spb)->default_value(0), "samples per buffer (0=auto, must be >= SPP)")
    ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    // Print help message
    if (vm.count("help")) {
        std::cout << "UHD CHDR Packet Capture and Analysis Tool" << std::endl;
        std::cout << desc << std::endl;
        std::cout << std::endl;
        std::cout << "Performance Tips:" << std::endl;
        std::cout << "  - CRITICAL: SPP must be >= 200 (preferably 500+)" << std::endl;
        std::cout << "  - SPP is set in stream arguments, not radio block properties" << std::endl;
        std::cout << "  - For 1GigE: Use --spp 500-1000, ensure MTU=1500" << std::endl;
        std::cout << "  - For 10GigE: Use --spp 1000-4000, ensure MTU=9000" << std::endl;
        std::cout << "  - Small SPP causes network flooding and overflows!" << std::endl;
        std::cout << "  - Higher rates need proportionally larger SPP" << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  Basic capture: " << argv[0] << " --rate 1e6 --freq 2.4e9" << std::endl;
        std::cout << "  Good performance: " << argv[0] << " --rate 1e6 --freq 2.4e9 --spp 500" << std::endl;
        std::cout << "  High rate: " << argv[0] << " --rate 10e6 --freq 2.4e9 --spp 1000" << std::endl;
        std::cout << "  With analysis: " << argv[0] << " --rate 1e6 --spp 500 --csv analysis.csv" << std::endl;
        return EXIT_SUCCESS;
    }
    
    // If analyze-only mode, just analyze the file
    if (analyze_only) {
        if (csv_file.empty()) {
            csv_file = file + ".csv";
        }
        analyze_capture_file(file, csv_file);
        return EXIT_SUCCESS;
    }
    
    // Calculate optimal SPP and SPB if not specified
    if (!vm.count("spp")) {
        // Auto-calculate reasonable SPP based on rate and connection type
        // X310 with 1GigE needs larger packets to avoid overhead
        if (rate <= 1e6) {
            spp = 500;   // For low rates, moderate packet size
        } else if (rate <= 5e6) {
            spp = 1000;  // For medium rates
        } else if (rate <= 25e6) {
            spp = 2000;  // For higher rates, larger packets
        } else {
            spp = 4000;  // For very high rates
        }
        std::cout << "Auto-calculated SPP: " << spp << " (use --spp to override)" << std::endl;
        std::cout << "  (Based on rate=" << rate/1e6 << " Msps)" << std::endl;
    }
    
    if (spb == 0) {
        // Auto-calculate SPB as multiple of SPP, but reasonable size
        spb = std::max((size_t)1000, spp * 2);  // At least 1000 samples, or 2x SPP
        spb = std::min(spb, (size_t)20000);     // But not more than 20k samples
        std::cout << "Auto-calculated SPB: " << spb << " (use --spb to override)" << std::endl;
    }
    
    // Validate parameters and enforce minimums for performance
    if (spp > 10000) {
        std::cerr << "Warning: Very large SPP (" << spp << ") may cause performance issues" << std::endl;
    }
    if (spp < 100) {
        std::cerr << "ERROR: SPP too small (" << spp << ") - minimum recommended is 200" << std::endl;
        std::cerr << "Small SPP values cause packet overhead and network flooding!" << std::endl;
        std::cerr << "For 1GigE: use SPP 200-1000, for 10GigE: use SPP 1000-4000" << std::endl;
        return EXIT_FAILURE;
    }
    if (spb < spp) {
        std::cerr << "ERROR: SPB (" << spb << ") must be >= SPP (" << spp << ")" << std::endl;
        return EXIT_FAILURE;
    }
    
    // Set up Ctrl+C handler
    std::signal(SIGINT, &sig_int_handler);
    if (num_packets == 0) {
        std::cout << "Press Ctrl+C to stop capture..." << std::endl;
    }
    
    // Create RFNoC graph
    std::cout << "\nCreating RFNoC graph with args: " << args << std::endl;
    auto graph = uhd::rfnoc::rfnoc_graph::make(args);
    
    // Print graph info if requested
    if (show_graph) {
        print_graph_info(graph);
    }
    
    // Get radio block
    uhd::rfnoc::block_id_t radio_ctrl_id(0, "Radio", radio_id);
    auto radio_ctrl = graph->get_block<uhd::rfnoc::radio_control>(radio_ctrl_id);
    
    // Configure radio
    std::cout << "Using radio " << radio_id << ", channel " << radio_chan << std::endl;

    // Wait for lo_locked before setting frequency
    std::cout << "Waiting for \"lo_locked\": " << std::flush;
    while (not radio_ctrl->get_rx_sensor("lo_locked", radio_chan).to_bool()) {
        std::this_thread::sleep_for(50ms);
        std::cout << "+" << std::flush;
    }
    std::cout << " locked." << std::endl;

    radio_ctrl->set_rx_frequency(freq, radio_chan);
    radio_ctrl->set_rx_gain(gain, radio_chan);
    if (bw > 0) {
        radio_ctrl->set_rx_bandwidth(bw, radio_chan);
    }
    
    // Find the RX path and configure blocks
    uhd::rfnoc::block_id_t last_block_id = radio_ctrl_id;
    size_t last_port = radio_chan;
    
    uhd::rfnoc::block_id_t last_block_in_chain;
    size_t last_port_in_chain;
    {
        auto edges = uhd::rfnoc::get_block_chain(graph, radio_ctrl_id, radio_chan, true);
        last_block_in_chain = edges.back().src_blockid;
        last_port_in_chain  = edges.back().src_port;
        if (edges.size() > 1) {
            uhd::rfnoc::connect_through_blocks(graph,
                radio_ctrl_id,
                radio_chan,
                last_block_in_chain,
                last_port_in_chain);
        }
    }

    last_block_id = last_block_in_chain;
    last_port = last_port_in_chain;

    // Create RX streamer with proper stream args
    uhd::stream_args_t stream_args(format, "sc16");
    stream_args.channels = {0};
    
    // CRITICAL: Set SPP in stream arguments, not on radio block
    // This is the correct way to control packet size in RFNoC
    stream_args.args["spp"] = std::to_string(spp);
    
    std::cout << "Setting SPP in stream arguments: " << spp << std::endl;
    
    std::cout << "Creating RX streamer with format=" << format << ", otw=sc16, spp=" << spp << std::endl;
    auto rx_stream = graph->create_rx_streamer(1, stream_args);
    
    // Verify the actual SPP used by the streamer
    size_t max_spp = rx_stream->get_max_num_samps();
    std::cout << "Streamer max samples per packet: " << max_spp << std::endl;
    
    // Adjust SPB if needed based on actual max SPP
    if (spb > max_spp) {
        spb = max_spp;
        std::cout << "SPB adjusted to max possible: " << spb << std::endl;
    }
    
    // Connect streamer to last block in chain
    std::cout << "Connecting " << last_block_id << ":" << last_port << " to streamer..." << std::endl;
    graph->connect(last_block_id, last_port, rx_stream, 0);
    
    // Commit the graph
    graph->commit();
    
    // Print active connections
    std::cout << "\nActive connections:" << std::endl;
    for (auto& edge : graph->enumerate_active_connections()) {
        std::cout << "  * " << edge.to_string() << std::endl;
    }
    
    // Set sample rate
    std::cout << "\nRequesting RX Rate: " << (rate / 1e6) << " Msps..." << std::endl;
    
    // Look for DDC in the path and set rate appropriately
    uhd::rfnoc::ddc_block_control::sptr ddc_ctrl;
    size_t ddc_chan = 0;
    
    auto edges = uhd::rfnoc::get_block_chain(graph, radio_ctrl_id, radio_chan, true);
    for (const auto& edge : edges) {
        auto block_id = uhd::rfnoc::block_id_t(edge.src_blockid);
        if (block_id.match("DDC")) {
            ddc_ctrl = graph->get_block<uhd::rfnoc::ddc_block_control>(block_id);
            ddc_chan = edge.src_port;
            break;
        }
    }
    
    if (ddc_ctrl) {
        std::cout << "Setting rate on DDC block" << std::endl;
        rate = ddc_ctrl->set_output_rate(rate, ddc_chan);
    } else {
        std::cout << "Setting rate on radio block" << std::endl;
        rate = radio_ctrl->set_rate(rate);
    }
    
    std::cout << "Actual RX Rate: " << (rate / 1e6) << " Msps" << std::endl;
    
    // Print final streaming configuration
    std::cout << "\n=== Final Streaming Configuration ===" << std::endl;
    std::cout << "Sample rate: " << rate/1e6 << " Msps" << std::endl;
    std::cout << "Requested samples per packet: " << spp << std::endl;
    std::cout << "Samples per buffer: " << spb << std::endl;
    std::cout << "Max samples per packet (from streamer): " << rx_stream->get_max_num_samps() << std::endl;
    std::cout << "Sample format: " << format << std::endl;
    std::cout << "CHDR width: " << chdr_width << " bits" << std::endl;
    
    // Calculate packet efficiency based on actual max SPP
    size_t actual_max_spp = rx_stream->get_max_num_samps();
    size_t effective_spp = std::min(spp, actual_max_spp);
    size_t bytes_per_sample = (format == "sc16") ? 4 : (format == "fc32") ? 8 : 16;
    size_t payload_bytes = effective_spp * bytes_per_sample;
    size_t header_bytes = (chdr_width / 8) + 8; // CHDR header + timestamp
    size_t total_packet_bytes = payload_bytes + header_bytes;
    double efficiency = (double)payload_bytes / total_packet_bytes * 100.0;
    
    std::cout << "\nPacket Efficiency Analysis:" << std::endl;
    std::cout << "  Effective SPP: " << effective_spp << " samples" << std::endl;
    std::cout << "  Payload: " << payload_bytes << " bytes" << std::endl;
    std::cout << "  Headers: " << header_bytes << " bytes" << std::endl;
    std::cout << "  Total: " << total_packet_bytes << " bytes" << std::endl;
    std::cout << "  Efficiency: " << std::fixed << std::setprecision(1) << efficiency << "%" << std::endl;
    
    if (effective_spp < spp) {
        std::cout << "  NOTE: Requested SPP (" << spp << ") exceeded max (" << actual_max_spp << ")" << std::endl;
    }
    
    if (efficiency < 80.0) {
        std::cout << "  WARNING: Low efficiency! Consider increasing SPP" << std::endl;
    } else if (efficiency > 95.0) {
        std::cout << "  EXCELLENT: High efficiency!" << std::endl;
    } else {
        std::cout << "  GOOD: Reasonable efficiency" << std::endl;
    }
    
    // Start capture
    bool enable_analysis = !csv_file.empty();
    
    if (format == "sc16") {
        capture_chdr_packets<std::complex<short>>(
            rx_stream, file, num_packets, time_requested, 
            enable_analysis, csv_file, chdr_width, rate, spb);
    } else if (format == "fc32") {
        capture_chdr_packets<std::complex<float>>(
            rx_stream, file, num_packets, time_requested,
            enable_analysis, csv_file, chdr_width, rate, spb);
    } else if (format == "fc64") {
        capture_chdr_packets<std::complex<double>>(
            rx_stream, file, num_packets, time_requested,
            enable_analysis, csv_file, chdr_width, rate, spb);
    } else {
        throw std::runtime_error("Unsupported format: " + format);
    }
    
    return EXIT_SUCCESS;
}