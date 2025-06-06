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

// Raw CHDR packet capture function
template <typename samp_type>
void capture_chdr_packets(
    uhd::rx_streamer::sptr rx_stream,
    const std::string& file,
    size_t num_packets,
    double time_requested,
    bool enable_analysis,
    const std::string& csv_file,
    size_t chdr_width_bits,
    double rate)
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

    // Prepare buffers
    const size_t max_samps_per_packet = rx_stream->get_max_num_samps();
    const size_t chdr_width_bytes = chdr_width_bits / 8;

    // Buffer for raw packet data
    std::vector<uint8_t> packet_buffer;
    packet_buffer.reserve(max_samps_per_packet * sizeof(samp_type) + 1024);

    // Metadata
    uhd::rx_metadata_t md;

    // Setup streaming
    uhd::stream_cmd_t stream_cmd(num_packets == 0 ? 
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS :
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);

    stream_cmd.num_samps = num_packets * max_samps_per_packet;
    stream_cmd.stream_now = (time_requested == 0.0);
    if (!stream_cmd.stream_now) {
        stream_cmd.time_spec = uhd::time_spec_t(time_requested);
    }

    rx_stream->issue_stream_cmd(stream_cmd);

    // Packet capture statistics
    size_t packets_captured = 0;
    size_t total_bytes = 0;
    auto start_time = std::chrono::steady_clock::now();

    std::cout << "Starting CHDR packet capture..." << std::endl;
    std::cout << "CHDR Width: " << chdr_width_bits << " bits" << std::endl;
    std::cout << "Max samples per packet: " << max_samps_per_packet << std::endl;

    // For analysis
    std::vector<chdr_packet_data> captured_packets;

    // Main capture loop
    while (!stop_signal_called && (num_packets == 0 || packets_captured < num_packets)) {
        // Create buffer vector for recv
        std::vector<samp_type> buff(max_samps_per_packet);
        std::vector<void*> buff_ptrs;
        buff_ptrs.push_back(&buff.front());

        // Receive samples
        size_t num_rx_samps = rx_stream->recv(
            buff_ptrs, max_samps_per_packet, md, 3.0);

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << "Timeout while streaming" << std::endl;
            break;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            std::cerr << "Overflow detected" << std::endl;
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "Receiver error: " << md.strerror() << std::endl;
            break;
        }

        // Calculate actual packet size (samples + CHDR overhead)
        size_t sample_size_bytes = num_rx_samps * sizeof(samp_type);
        size_t header_size = (chdr_width_bits / 8);
        size_t packet_size_bytes = sample_size_bytes + header_size;

        // Construct CHDR packet in buffer
        packet_buffer.clear();

        // Add CHDR header
        uint64_t header = 0;
        header |= (uint64_t)(0) & 0xFFFF;                    // DstEPID (placeholder)
        header |= ((uint64_t)packet_size_bytes & 0xFFFF) << 16;  // Length
        header |= ((uint64_t)(packets_captured & 0xFFFF)) << 32; // SeqNum
        header |= ((uint64_t)0 & 0x1F) << 48;                // NumMData
        header |= ((uint64_t)(md.has_time_spec ? 0x7 : 0x6) & 0x7) << 53; // PktType
        header |= ((uint64_t)(md.end_of_burst ? 1 : 0) & 0x1) << 57;  // EOB
        header |= ((uint64_t)0 & 0x3F) << 58;                // VC

        // Write header to buffer
        packet_buffer.resize(8);
        memcpy(packet_buffer.data(), &header, 8);

        // Add timestamp if present
        if (md.has_time_spec) {
            uint64_t timestamp = md.time_spec.to_ticks(rate);
            packet_buffer.resize(packet_buffer.size() + 8);
            memcpy(packet_buffer.data() + 8, &timestamp, 8);
            packet_size_bytes += 8;
        }

        // Add sample data
        size_t data_offset = packet_buffer.size();
        packet_buffer.resize(packet_buffer.size() + sample_size_bytes);
        memcpy(packet_buffer.data() + data_offset, buff.data(), sample_size_bytes);

        // Write packet size followed by packet data - FIXED CASTING
        uint32_t pkt_size = static_cast<uint32_t>(packet_size_bytes);
        outfile.write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
        outfile.write(reinterpret_cast<const char*>(packet_buffer.data()), packet_size_bytes);

        // Store for analysis if requested
        if (enable_analysis) {
            chdr_packet_data pkt;

            // Parse based on CHDR width
            const uint64_t* data_ptr = reinterpret_cast<const uint64_t*>(packet_buffer.data());
            pkt.header = data_ptr[0];

            if (md.has_time_spec && chdr_width_bits == 64) {
                pkt.timestamp = data_ptr[1];
            } else if (md.has_time_spec && chdr_width_bits > 64) {
                pkt.timestamp = data_ptr[1]; // Timestamp is in second 64-bit word
            }

            // Store payload
            size_t header_offset = (chdr_width_bits == 64 && md.has_time_spec) ? 16 : chdr_width_bytes;
            pkt.payload.assign(packet_buffer.begin() + header_offset, 
                             packet_buffer.begin() + packet_size_bytes);

            captured_packets.push_back(pkt);
        }

        packets_captured++;
        total_bytes += packet_size_bytes;

        // Progress update
        if (packets_captured % 1000 == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            double rate = total_bytes * 1e9 / 
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
            std::cout << "\rCaptured " << packets_captured << " packets, "
                      << total_bytes/1024.0/1024.0 << " MB, "
                      << rate/1024.0/1024.0 << " MB/s" << std::flush;
        }
    }

    std::cout << std::endl;
    outfile.close();

    // Stop streaming
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(stream_cmd);

    // Report statistics
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    double elapsed_secs = std::chrono::duration<double>(elapsed).count();

    std::cout << "\nCapture Statistics:" << std::endl;
    std::cout << "  Total packets captured: " << packets_captured << std::endl;
    std::cout << "  Total data captured: " << total_bytes/1024.0/1024.0 << " MB" << std::endl;
    std::cout << "  Average data rate: " << (total_bytes/elapsed_secs)/1024.0/1024.0 << " MB/s" << std::endl;
    std::cout << "  Capture duration: " << elapsed_secs << " seconds" << std::endl;

    // Perform analysis if requested
    if (enable_analysis && !csv_file.empty()) {
        std::cout << "\nAnalyzing captured packets..." << std::endl;
        analyze_packets(captured_packets, csv_file, chdr_width_bits);
    }
}

// Analyze captured packets and write to CSV
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
        << "dst_epid,has_timestamp,timestamp,payload_size,payload_sample" << std::endl;

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

        // Sample of payload data (first 8 bytes as hex)
        if (pkt.payload.size() >= 8) {
            csv << "0x";
            for (int j = 0; j < 8; ++j) {
                csv << std::hex << std::setw(2) << static_cast<int>(pkt.payload[j]);
            }
        } else {
            csv << "N/A";
        }

        csv << std::endl;
    }

    csv.close();
    std::cout << "Analysis complete. Results written to: " << csv_file << std::endl;
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

// Main function
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // Variables for program options
    std::string args, file, format, csv_file, block_id;
    size_t radio_id, radio_chan, block_port;
    size_t num_packets, chdr_width;
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
        ("block-id", po::value<std::string>(&block_id), "optional block to insert in chain")
        ("block-port", po::value<size_t>(&block_port)->default_value(0), "port on optional block")
        ("format", po::value<std::string>(&format)->default_value("sc16"), "sample format (sc16, fc32, fc64)")
        ("chdr-width", po::value<size_t>(&chdr_width)->default_value(64), "CHDR width in bits")
        ("time", po::value<double>(&time_requested)->default_value(0.0), "time to start capture")
        ("show-graph", po::value<bool>(&show_graph)->default_value(false), "print RFNoC graph info")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // Print help message
    if (vm.count("help")) {
        std::cout << "UHD CHDR Packet Capture and Analysis Tool" << std::endl;
        std::cout << desc << std::endl;
        std::cout << std::endl;
        std::cout << "This tool captures data from RFNoC blocks and stores it with CHDR formatting." << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  Capture packets: " << argv[0] << " --rate 10e6 --freq 100e6 --num-packets 1000" << std::endl;
        std::cout << "  Capture and analyze: " << argv[0] << " --rate 10e6 --csv analysis.csv" << std::endl;
        std::cout << "  Analyze existing file: " << argv[0] << " --analyze-only --file capture.dat --csv analysis.csv" << std::endl;
        std::cout << "  Show graph info: " << argv[0] << " --show-graph" << std::endl;
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

    // Set up Ctrl+C handler
    std::signal(SIGINT, &sig_int_handler);
    if (num_packets == 0) {
        std::cout << "Press Ctrl+C to stop capture..." << std::endl;
    }

    // Create RFNoC graph
    std::cout << "Creating RFNoC graph with args: " << args << std::endl;
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
    
    // Wait for lo_locked before setting frequency (similar to working example)
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

    // CRITICAL FIX: Connect all blocks in the chain properly
    // Find the complete path from radio to SEP
    auto block_chain = uhd::rfnoc::get_block_chain(
        graph, radio_ctrl_id, radio_chan, true /* source chain */);

    if (block_chain.empty()) {
        throw uhd::runtime_error("No path found from radio to SEP!");
    }

    // Find the last block before SEP
    uhd::rfnoc::block_id_t last_block_id;
    size_t last_port = 0;
    
    for (const auto& edge : block_chain) {
        if (edge.dst_blockid.match("SEP")) {
            // This is the edge connecting to SEP, use the source block
            last_block_id = uhd::rfnoc::block_id_t(edge.src_blockid);
            last_port = edge.src_port;
            break;
        }
    }

    if (last_block_id.to_string().empty()) {
        throw uhd::runtime_error("Could not find block connecting to SEP!");
    }

    std::cout << "Last block in chain: " << last_block_id << ":" << last_port << std::endl;

    // Create RX streamer
    uhd::stream_args_t stream_args(format, "sc16");
    stream_args.channels = {0};  // Single channel streamer

    std::cout << "Creating RX streamer..." << std::endl;
    auto rx_stream = graph->create_rx_streamer(1, stream_args);

    // CRITICAL: Connect through all blocks from radio to streamer
    // This ensures all static connections are activated
    std::cout << "Connecting " << radio_ctrl_id << ":" << radio_chan 
              << " through chain to streamer..." << std::endl;
    
    // Connect through blocks ensures all intermediate connections are made
    uhd::rfnoc::connect_through_blocks(
        graph, radio_ctrl_id, radio_chan, rx_stream, 0);

    // Commit the graph
    graph->commit();

    // Print active connections
    std::cout << "\nActive connections:" << std::endl;
    for (auto& edge : graph->enumerate_active_connections()) {
        std::cout << "  * " << edge.to_string() << std::endl;
    }
    std::cout << std::endl;

    // Set sample rate on the appropriate block
    std::cout << "Requesting RX Rate: " << (rate / 1e6) << " Msps..." << std::endl;

    // Check if we have a DDC in the path
    uhd::rfnoc::ddc_block_control::sptr ddc_ctrl;
    size_t ddc_chan = 0;

    for (const auto& edge : block_chain) {
        auto block_id = uhd::rfnoc::block_id_t(edge.src_blockid);
        if (block_id.match("DDC")) {
            ddc_ctrl = graph->get_block<uhd::rfnoc::ddc_block_control>(block_id);
            ddc_chan = edge.src_port;
            break;
        }
    }

    if (ddc_ctrl) {
        std::cout << "Setting rate on DDC block!" << std::endl;
        rate = ddc_ctrl->set_output_rate(rate, ddc_chan);
    } else {
        std::cout << "Setting rate on radio block" << std::endl;
        rate = radio_ctrl->set_rate(rate);
    }

    std::cout << "Actual RX Rate: " << (rate / 1e6) << " Msps..." << std::endl;

    // Capture packets
    bool enable_analysis = !csv_file.empty();

    if (format == "sc16") {
        capture_chdr_packets<std::complex<int16_t>>(
            rx_stream, file, num_packets, time_requested, 
            enable_analysis, csv_file, chdr_width, rate);
    } else if (format == "fc32") {
        capture_chdr_packets<std::complex<float>>(
            rx_stream, file, num_packets, time_requested,
            enable_analysis, csv_file, chdr_width, rate);
    } else if (format == "fc64") {
        capture_chdr_packets<std::complex<double>>(
            rx_stream, file, num_packets, time_requested,
            enable_analysis, csv_file, chdr_width, rate);
    } else {
        throw std::runtime_error("Unsupported format: " + format);
    }

    return EXIT_SUCCESS;
}