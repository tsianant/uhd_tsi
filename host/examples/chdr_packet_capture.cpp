//
// Copyright 2024 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CHDR Packet Capture Tool - Wire Format Compliant
//
// This utility captures sample data via UHD streaming API and formats it
// according to the actual CHDR wire format specification with proper
// little-endian encoding and correct header structure.
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
#include <algorithm>

namespace po = boost::program_options;
using namespace std::chrono_literals;

// Global flag for Ctrl+C handling
static volatile bool stop_signal_called = false;

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

// Analyze packets and write to CSV
void analyze_packets(const std::vector<chdr_packet_data>& packets, 
                    const std::string& csv_file,
                    size_t chdr_width_bits,
                    double tick_rate)
{
    std::ofstream csv(csv_file);
    if (!csv.is_open()) {
        throw std::runtime_error("Failed to open CSV file: " + csv_file);
    }
    
    // Write CSV header
    csv << "packet_num,vc,eob,eov,pkt_type,pkt_type_str,num_mdata,seq_num,length,"
        << "dst_epid,has_timestamp,timestamp_ticks,timestamp_sec,payload_size,"
        << "num_samples,first_4_bytes_hex,sample_0_real,sample_0_imag,"
        << "sample_1_real,sample_1_imag" << std::endl;
    
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
                << std::fixed << std::setprecision(9) << timestamp_sec;
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
        
        // Decode first two sc16 samples
        if (pkt.payload.size() >= 8) {
            // Read as little-endian int16 values
            int16_t real0 = read_le<int16_t>(&pkt.payload[0]);
            int16_t imag0 = read_le<int16_t>(&pkt.payload[2]);
            int16_t real1 = read_le<int16_t>(&pkt.payload[4]);
            int16_t imag1 = read_le<int16_t>(&pkt.payload[6]);
            
            csv << "," << std::dec << real0 << "," << imag0
                << "," << real1 << "," << imag1;
        } else {
            csv << ",N/A,N/A,N/A,N/A";
        }
        
        csv << std::endl;
    }
    
    csv.close();
    std::cout << "Analysis complete. Results written to: " << csv_file << std::endl;
}

// Fixed capture function with proper wire format
template <typename samp_type>
void capture_chdr_packets(
    uhd::rx_streamer::sptr rx_stream,
    uhd::rfnoc::radio_control::sptr radio_ctrl,
    const std::string& file,
    size_t num_packets,
    double time_requested,
    bool enable_analysis,
    const std::string& csv_file,
    size_t chdr_width_bits,
    double rate,
    size_t samps_per_buff,
    uint16_t dst_epid = 0)  // Destination EPID (could be stream endpoint ID)
{
    // Get the device tick rate for proper timestamp conversion
    double tick_rate = radio_ctrl->get_tick_rate();
    std::cout << "Device tick rate: " << tick_rate/1e6 << " MHz" << std::endl;
    
    // Open binary file for writing
    std::ofstream outfile(file.c_str(), std::ios::binary);
    if (!outfile.is_open()) {
        throw std::runtime_error("Failed to open file: " + file);
    }
    
    // Write file header with tick rate
    struct {
        uint32_t magic;
        uint32_t version;
        uint32_t chdr_width;
        double tick_rate;
    } file_header;
    
    file_header.magic = 0x43484452; // "CHDR"
    file_header.version = 2;         // Version 2 includes tick rate
    file_header.chdr_width = chdr_width_bits;
    file_header.tick_rate = tick_rate;
    
    outfile.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    
    // Get streaming parameters
    const size_t max_samps_per_packet = rx_stream->get_max_num_samps();
    size_t effective_spb = std::min(samps_per_buff, max_samps_per_packet);
    
    std::cout << "\nStream configuration:" << std::endl;
    std::cout << "  Sample rate: " << rate/1e6 << " Msps" << std::endl;
    std::cout << "  Tick rate: " << tick_rate/1e6 << " MHz" << std::endl;
    std::cout << "  Max samples per packet: " << max_samps_per_packet << std::endl;
    std::cout << "  Effective samples per buffer: " << effective_spb << std::endl;
    std::cout << "  Sample size: " << sizeof(samp_type) << " bytes" << std::endl;
    std::cout << "  CHDR width: " << chdr_width_bits << " bits" << std::endl;
    
    // Metadata
    uhd::rx_metadata_t md;
    
    // Setup streaming
    uhd::stream_cmd_t stream_cmd(num_packets == 0 ? 
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS :
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    
    if (num_packets > 0) {
        stream_cmd.num_samps = num_packets * effective_spb;
        std::cout << "Requesting " << stream_cmd.num_samps << " total samples" << std::endl;
    }
    
    stream_cmd.stream_now = (time_requested == 0.0);
    if (!stream_cmd.stream_now) {
        stream_cmd.time_spec = uhd::time_spec_t(time_requested);
    }
    
    rx_stream->issue_stream_cmd(stream_cmd);
    
    // Statistics
    size_t packets_captured = 0;
    size_t total_samples = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // For analysis
    std::vector<chdr_packet_data> captured_packets;
    
    std::cout << "\nStarting CHDR packet capture..." << std::endl;
    
    // Main capture loop
    while (!stop_signal_called && (num_packets == 0 || packets_captured < num_packets)) {
        // Receive samples
        std::vector<samp_type> buff(effective_spb);
        std::vector<void*> buff_ptrs = {&buff.front()};
        
        size_t num_rx_samps = rx_stream->recv(buff_ptrs, effective_spb, md, 3.0, false);
        
        // Handle errors
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << "\nTimeout while streaming" << std::endl;
            break;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            std::cerr << "O" << std::flush;
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "\nReceiver error: " << md.strerror() << std::endl;
            break;
        }

        std::cout << "THe metadata we found in pakcets" << md.to_pp_string(0) <<std::endl;
        
        if (num_rx_samps == 0) continue;
        
        // Build CHDR packet in wire format (little-endian)
        std::vector<uint8_t> packet_buffer;
        
        // Calculate sizes
        size_t payload_bytes = num_rx_samps * sizeof(samp_type);
        size_t header_bytes = 8;
        size_t timestamp_bytes = md.has_time_spec ? 8 : 0;
        size_t metadata_bytes = 0; // No metadata for data packets
        size_t total_chdr_bytes = header_bytes + timestamp_bytes + metadata_bytes + payload_bytes;
        
        // Build header (64-bit, little-endian)
        uint64_t header = 0;
        header |= (uint64_t)dst_epid & 0xFFFF;                    // Destination EPID
        header |= ((uint64_t)total_chdr_bytes & 0xFFFF) << 16;    // Length (entire CHDR packet)
        header |= ((uint64_t)packets_captured & 0xFFFF) << 32;    // Sequence number
        header |= ((uint64_t)0 & 0x1F) << 48;                     // NumMData (0 for data)
        header |= ((uint64_t)(md.has_time_spec ? PKT_TYPE_DATA_WITH_TS : PKT_TYPE_DATA_NO_TS) & 0x7) << 53;
        header |= ((uint64_t)0 & 0x1) << 56;                      // EOV
        header |= ((uint64_t)(md.end_of_burst ? 1 : 0) & 0x1) << 57; // EOB
        header |= ((uint64_t)0 & 0x3F) << 58;                     // VC
        
        // Write header (little-endian)
        write_le(packet_buffer, header);
        
        // Write timestamp if present (little-endian)
        if (md.has_time_spec) {
            uint64_t timestamp_ticks = md.time_spec.to_ticks(tick_rate);
            write_le(packet_buffer, timestamp_ticks);
        }
        
        // Write payload (samples are already in correct endianness)
        const uint8_t* sample_bytes = reinterpret_cast<const uint8_t*>(buff.data());
        packet_buffer.insert(packet_buffer.end(), sample_bytes, sample_bytes + payload_bytes);
        
        // Write to file: [packet_size][packet_data]
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
        if (packets_captured % 10 == 0) {
            std::cout << "\rPackets: " << packets_captured 
                      << ", Samples: " << total_samples << std::flush;
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
    std::cout << "Total packets captured: " << packets_captured << std::endl;
    std::cout << "Total samples: " << total_samples << std::endl;
    std::cout << "Capture duration: " << elapsed_secs << " seconds" << std::endl;
    std::cout << "Average sample rate: " << (total_samples/elapsed_secs)/1e6 << " Msps" << std::endl;
    
    // Perform analysis
    if (enable_analysis && !csv_file.empty() && !captured_packets.empty()) {
        std::cout << "\nAnalyzing packets..." << std::endl;
        analyze_packets(captured_packets, csv_file, chdr_width_bits, tick_rate);
    }
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
    struct {
        uint32_t magic;
        uint32_t version;
        uint32_t chdr_width;
        double tick_rate;
    } file_header;
    
    // Try to read new format first
    infile.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
    
    if (file_header.magic != 0x43484452) {
        throw std::runtime_error("Invalid file format");
    }
    
    // Handle old format without tick rate
    if (file_header.version == 0) {
        // Old format - reread as old structure
        infile.seekg(0);
        uint32_t old_header[2];
        infile.read(reinterpret_cast<char*>(old_header), sizeof(old_header));
        file_header.magic = old_header[0];
        file_header.chdr_width = old_header[1];
        file_header.tick_rate = 200e6; // Assume 200 MHz
        std::cout << "Warning: Old file format, assuming 200 MHz tick rate" << std::endl;
    }
    
    std::cout << "CHDR capture file analysis" << std::endl;
    std::cout << "CHDR Width: " << file_header.chdr_width << " bits" << std::endl;
    std::cout << "Tick Rate: " << file_header.tick_rate/1e6 << " MHz" << std::endl;
    
    std::vector<chdr_packet_data> packets;
    
    // Read packets
    while (!infile.eof()) {
        uint32_t pkt_size;
        infile.read(reinterpret_cast<char*>(&pkt_size), sizeof(pkt_size));
        
        if (infile.eof() || pkt_size == 0 || pkt_size > 65536) break;
        
        std::vector<uint8_t> packet_buffer(pkt_size);
        infile.read(reinterpret_cast<char*>(packet_buffer.data()), pkt_size);
        
        if (infile.gcount() != pkt_size) {
            std::cerr << "Warning: Incomplete packet at end of file" << std::endl;
            break;
        }
        
        // Parse packet
        chdr_packet_data pkt;
        
        // Read header (little-endian)
        pkt.header_raw = read_le<uint64_t>(&packet_buffer[0]);
        pkt.parse_header();
        
        size_t offset = 8;
        
        // Read timestamp if present
        if (pkt.has_timestamp) {
            if (packet_buffer.size() < offset + 8) {
                std::cerr << "Packet too small for timestamp" << std::endl;
                continue;
            }
            pkt.timestamp = read_le<uint64_t>(&packet_buffer[offset]);
            offset += 8;
        }
        
        // Read metadata
        for (size_t i = 0; i < pkt.num_mdata; i++) {
            if (packet_buffer.size() < offset + 8) break;
            uint64_t mdata = read_le<uint64_t>(&packet_buffer[offset]);
            pkt.metadata.push_back(mdata);
            offset += 8;
        }
        
        // Rest is payload
        if (offset < packet_buffer.size()) {
            pkt.payload.assign(packet_buffer.begin() + offset, packet_buffer.end());
        }
        
        packets.push_back(pkt);
    }
    
    infile.close();
    
    std::cout << "Read " << packets.size() << " packets from file" << std::endl;
    
    // Print some statistics
    if (!packets.empty()) {
        size_t total_samples = 0;
        size_t data_packets = 0;
        
        for (const auto& pkt : packets) {
            if (pkt.pkt_type == PKT_TYPE_DATA_WITH_TS || pkt.pkt_type == PKT_TYPE_DATA_NO_TS) {
                data_packets++;
                total_samples += pkt.payload.size() / 4; // Assuming sc16
            }
        }
        
        std::cout << "Data packets: " << data_packets << std::endl;
        std::cout << "Total samples: " << total_samples << std::endl;
        
        // Check sequence numbers
        bool seq_error = false;
        for (size_t i = 1; i < packets.size(); i++) {
            uint16_t expected = (packets[i-1].seq_num + 1) & 0xFFFF;
            if (packets[i].seq_num != expected) {
                std::cout << "Sequence error at packet " << i 
                          << ": expected " << expected 
                          << ", got " << packets[i].seq_num << std::endl;
                seq_error = true;
            }
        }
        if (!seq_error) {
            std::cout << "Sequence numbers are continuous" << std::endl;
        }
    }
    
    // Analyze packets
    analyze_packets(packets, csv_file, file_header.chdr_width, file_header.tick_rate);
}

// Print graph info
void print_graph_info(uhd::rfnoc::rfnoc_graph::sptr graph)
{
    std::cout << "\n=== RFNoC Graph Information ===" << std::endl;
    
    std::cout << "\nRFNoC blocks:" << std::endl;
    auto block_ids = graph->find_blocks("");
    for (const auto& block_id : block_ids) {
        std::cout << "  * " << block_id.to_string() << std::endl;
    }
    
    std::cout << "\nStatic connections:" << std::endl;
    auto edges = graph->enumerate_static_connections();
    for (const auto& edge : edges) {
        std::cout << "  * " << edge.to_string() << std::endl;
    }
}

// Main function
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // Variables
    std::string args, file, format, csv_file;
    size_t radio_id, radio_chan;
    size_t num_packets, chdr_width, spp, spb;
    double rate, freq, gain, bw, time_requested;
    bool analyze_only = false;
    bool show_graph = false;
    
    // Setup program options
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
        ("format", po::value<std::string>(&format)->default_value("sc16"), "sample format (sc16, fc32, fc64)")
        ("chdr-width", po::value<size_t>(&chdr_width)->default_value(64), "CHDR width in bits")
        ("time", po::value<double>(&time_requested)->default_value(0.0), "time to start capture")
        ("show-graph", po::value<bool>(&show_graph)->default_value(false), "print RFNoC graph info")
        ("spp", po::value<size_t>(&spp), "samples per packet")
        ("spb", po::value<size_t>(&spb)->default_value(0), "samples per buffer (0=auto)")
    ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    // Help message
    if (vm.count("help")) {
        std::cout << "CHDR Packet Capture Tool - Wire Format Compliant" << std::endl;
        std::cout << desc << std::endl;
        std::cout << "\nThis tool captures data via UHD streaming and formats it" << std::endl;
        std::cout << "according to the CHDR wire format specification." << std::endl;
        std::cout << "\nExamples:" << std::endl;
        std::cout << "  Capture: " << argv[0] << " --rate 1e6 --freq 2.4e9 --spp 500" << std::endl;
        std::cout << "  Analyze: " << argv[0] << " --analyze-only --file capture.dat --csv analysis.csv" << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Analyze mode
    if (analyze_only) {
        if (csv_file.empty()) {
            csv_file = file + ".csv";
        }
        analyze_capture_file(file, csv_file);
        return EXIT_SUCCESS;
    }
    
    // Auto-calculate SPP if not specified
    if (!vm.count("spp")) {
        if (rate <= 1e6) {
            spp = 364;  // Good for 1GigE at low rates
        } else if (rate <= 5e6) {
            spp = 1000;
        } else {
            spp = 2000;
        }
        std::cout << "Auto-calculated SPP: " << spp << std::endl;
    }
    
    if (spb == 0) {
        spb = std::max((size_t)1000, spp * 2);
        spb = std::min(spb, (size_t)20000);
    }
    
    // Validate
    if (spp < 100) {
        std::cerr << "ERROR: SPP too small" << std::endl;
        return EXIT_FAILURE;
    }
    
    // Signal handler
    std::signal(SIGINT, &sig_int_handler);
    if (num_packets == 0) {
        std::cout << "Press Ctrl+C to stop capture..." << std::endl;
    }
    
    // Create graph
    std::cout << "\nCreating RFNoC graph..." << std::endl;
    auto graph = uhd::rfnoc::rfnoc_graph::make(args);
    
    if (show_graph) {
        print_graph_info(graph);
    }
    
    // Get radio
    uhd::rfnoc::block_id_t radio_ctrl_id(0, "Radio", radio_id);
    auto radio_ctrl = graph->get_block<uhd::rfnoc::radio_control>(radio_ctrl_id);
    
    // Configure radio
    std::cout << "Configuring radio " << radio_id << ", channel " << radio_chan << std::endl;
    radio_ctrl->set_rx_frequency(freq, radio_chan);
    radio_ctrl->set_rx_gain(gain, radio_chan);
    if (bw > 0) {
        radio_ctrl->set_rx_bandwidth(bw, radio_chan);
    }
    
    // Wait for LO lock
    std::cout << "Waiting for LO lock: " << std::flush;
    while (!radio_ctrl->get_rx_sensor("lo_locked", radio_chan).to_bool()) {
        std::this_thread::sleep_for(50ms);
        std::cout << "+" << std::flush;
    }
    std::cout << " locked." << std::endl;
    
    // Find RX path
    auto edges = uhd::rfnoc::get_block_chain(graph, radio_ctrl_id, radio_chan, true);
    auto last_block_id = edges.back().src_blockid;
    auto last_port = edges.back().src_port;
    
    if (edges.size() > 1) {
        uhd::rfnoc::connect_through_blocks(graph,
            radio_ctrl_id, radio_chan,
            last_block_id, last_port);
    }
    
    // Create streamer
    uhd::stream_args_t stream_args(format, "sc16");
    stream_args.channels = {0};
    stream_args.args["spp"] = std::to_string(spp);
    
    auto rx_stream = graph->create_rx_streamer(1, stream_args);
    graph->connect(last_block_id, last_port, rx_stream, 0);
    graph->commit();
    
    // Set rate
    std::cout << "Setting sample rate: " << rate/1e6 << " Msps..." << std::endl;
    
    // Check for DDC
    uhd::rfnoc::ddc_block_control::sptr ddc_ctrl;
    for (const auto& edge : edges) {
        auto block_id = uhd::rfnoc::block_id_t(edge.src_blockid);
        if (block_id.match("DDC")) {
            ddc_ctrl = graph->get_block<uhd::rfnoc::ddc_block_control>(block_id);
            rate = ddc_ctrl->set_output_rate(rate, edge.src_port);

            break;
        }
    }
    
    if (!ddc_ctrl) {
        rate = radio_ctrl->set_rate(rate);
    }
    
    std::cout << "Actual sample rate: " << rate/1e6 << " Msps" << std::endl;
    
    // Start capture
    bool enable_analysis = !csv_file.empty();
    
    if (format == "sc16") {
        capture_chdr_packets<std::complex<short>>(
            rx_stream, radio_ctrl, file, num_packets, time_requested, 
            enable_analysis, csv_file, chdr_width, rate, spb);
    } else if (format == "fc32") {
        capture_chdr_packets<std::complex<float>>(
            rx_stream, radio_ctrl, file, num_packets, time_requested,
            enable_analysis, csv_file, chdr_width, rate, spb);
    } else if (format == "fc64") {
        capture_chdr_packets<std::complex<double>>(
            rx_stream, radio_ctrl, file, num_packets, time_requested,
            enable_analysis, csv_file, chdr_width, rate, spb);
    } else {
        throw std::runtime_error("Unsupported format: " + format);
    }
    
    return EXIT_SUCCESS;
}