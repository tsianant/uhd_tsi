//
// Raw CHDR Network Packet Capture Tool
// 
// This tool captures actual CHDR packets from the network interface using raw sockets,
// providing true packet-level visibility into RFNoC communications.
//

#include <uhd/rfnoc_graph.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/utils/safe_main.hpp>
#include <boost/program_options.hpp>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>

namespace po = boost::program_options;

static volatile bool stop_signal_called = false;

void sig_int_handler(int) {
    stop_signal_called = true;
}

// CHDR header structure for 64-bit CHDR
struct chdr_header {
    uint64_t header_word;
    
    uint16_t dst_epid() const { return header_word & 0xFFFF; }
    uint16_t length() const { return (header_word >> 16) & 0xFFFF; }
    uint16_t seq_num() const { return (header_word >> 32) & 0xFFFF; }
    uint8_t num_mdata() const { return (header_word >> 48) & 0x1F; }
    uint8_t pkt_type() const { return (header_word >> 53) & 0x7; }
    bool eov() const { return (header_word >> 56) & 0x1; }
    bool eob() const { return (header_word >> 57) & 0x1; }
    uint8_t vc() const { return (header_word >> 58) & 0x3F; }
    
    bool has_timestamp() const { return pkt_type() == 0x7; }
};

// Function to parse and analyze a CHDR packet
void analyze_chdr_packet(const uint8_t* packet_data, size_t packet_len, 
                        std::ofstream& csv_file, size_t packet_num,
                        double tick_rate)
{
    if (packet_len < 8) return;
    
    // Parse CHDR header (little-endian)
    chdr_header hdr;
    hdr.header_word = *reinterpret_cast<const uint64_t*>(packet_data);
    
    // Parse timestamp if present
    uint64_t timestamp = 0;
    size_t payload_offset = 8;
    if (hdr.has_timestamp() && packet_len >= 16) {
        timestamp = *reinterpret_cast<const uint64_t*>(packet_data + 8);
        payload_offset = 16;
    }
    
    // Calculate payload size
    size_t payload_size = packet_len - payload_offset;
    
    // Get first sample (if sc16)
    int16_t first_real = 0, first_imag = 0;
    std::stringstream first_sample_hex;
    if (payload_size >= 4) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(packet_data + payload_offset);
        first_real = samples[0];
        first_imag = samples[1];
        
        // Format as hex
        first_sample_hex << "0x";
        for (int i = 0; i < 4 && i < payload_size; i++) {
            first_sample_hex << std::hex << std::setw(2) << std::setfill('0') 
                           << static_cast<unsigned>(packet_data[payload_offset + i]);
        }
    }
    
    // Write to CSV
    csv_file << packet_num << ","
             << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)hdr.vc() << ","
             << (hdr.eob() ? "1" : "0") << ","
             << (hdr.eov() ? "1" : "0") << ","
             << "0x" << (int)hdr.pkt_type() << ","
             << (hdr.pkt_type() == 0x7 ? "Data (With TS)" : 
                 hdr.pkt_type() == 0x6 ? "Data (No TS)" : "Other") << ","
             << std::dec << (int)hdr.num_mdata() << ","
             << hdr.seq_num() << ","
             << hdr.length() << ","
             << "0x" << std::hex << std::setw(4) << hdr.dst_epid() << ","
             << (hdr.has_timestamp() ? "1" : "0") << ",";
    
    if (hdr.has_timestamp()) {
        double timestamp_sec = static_cast<double>(timestamp) / tick_rate;
        csv_file << std::dec << timestamp << ","
                 << std::fixed << std::setprecision(9) << timestamp_sec;
    } else {
        csv_file << "N/A,N/A";
    }
    
    csv_file << "," << std::dec << payload_size << ","
             << "sc16," << payload_size/4 << ","
             << first_sample_hex.str() << ","
             << first_real << "," << first_imag << std::endl;
}

// Packet handler callback for pcap
void packet_handler(u_char* user_data, const struct pcap_pkthdr* pkthdr, const u_char* packet)
{
    struct capture_context {
        std::ofstream* outfile;
        std::ofstream* csv_file;
        size_t* packet_count;
        double tick_rate;
        uint16_t capture_port;
        bool verbose;
    };
    
    capture_context* ctx = reinterpret_cast<capture_context*>(user_data);
    
    // Skip if packet is too small for Ethernet + IP + UDP
    if (pkthdr->caplen < 42) return;
    
    // Parse Ethernet header (14 bytes)
    const struct iphdr* ip_header = reinterpret_cast<const struct iphdr*>(packet + 14);
    
    // Skip non-IP packets
    if (ip_header->version != 4) return;
    
    // Calculate IP header length
    size_t ip_hdr_len = ip_header->ihl * 4;
    
    // Parse UDP header
    const struct udphdr* udp_header = reinterpret_cast<const struct udphdr*>(
        packet + 14 + ip_hdr_len);
    
    // Check if it's our target port
    if (ntohs(udp_header->dest) != ctx->capture_port) return;
    
    // Calculate offsets
    size_t udp_payload_offset = 14 + ip_hdr_len + 8;
    size_t udp_payload_len = ntohs(udp_header->len) - 8;
    
    // Skip if not enough data for CHDR header
    if (pkthdr->caplen < udp_payload_offset + 8) return;
    
    // Get CHDR data
    const uint8_t* chdr_data = packet + udp_payload_offset;
    
    // Write packet to file
    uint32_t pkt_size = udp_payload_len;
    ctx->outfile->write(reinterpret_cast<const char*>(&pkt_size), sizeof(pkt_size));
    ctx->outfile->write(reinterpret_cast<const char*>(chdr_data), udp_payload_len);
    
    // Analyze packet
    if (ctx->csv_file) {
        analyze_chdr_packet(chdr_data, udp_payload_len, *ctx->csv_file, 
                          *ctx->packet_count, ctx->tick_rate);
    }
    
    (*ctx->packet_count)++;
    
    if (ctx->verbose && *ctx->packet_count % 100 == 0) {
        std::cout << "\rCaptured " << *ctx->packet_count << " packets" << std::flush;
    }
}

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    std::string interface, output_file, csv_file, filter;
    uint16_t port;
    size_t num_packets;
    double tick_rate;
    bool verbose;
    
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("interface", po::value<std::string>(&interface)->default_value("any"), 
         "network interface to capture on")
        ("port", po::value<uint16_t>(&port)->default_value(49155), 
         "UDP port for CHDR data (default: 49155)")
        ("file", po::value<std::string>(&output_file)->default_value("chdr_capture.dat"), 
         "output file")
        ("csv", po::value<std::string>(&csv_file)->default_value(""), 
         "CSV analysis file")
        ("num-packets", po::value<size_t>(&num_packets)->default_value(0), 
         "number of packets to capture (0=unlimited)")
        ("tick-rate", po::value<double>(&tick_rate)->default_value(200e6), 
         "device tick rate for timestamp conversion")
        ("filter", po::value<std::string>(&filter)->default_value(""), 
         "additional pcap filter")
        ("verbose", po::value<bool>(&verbose)->default_value(true), 
         "verbose output")
    ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    if (vm.count("help")) {
        std::cout << "Raw CHDR Network Packet Capture Tool" << std::endl;
        std::cout << desc << std::endl;
        std::cout << "\nThis tool captures actual CHDR packets from the network interface." << std::endl;
        std::cout << "It requires root/administrator privileges to capture raw packets." << std::endl;
        std::cout << "\nExample: sudo " << argv[0] << " --interface eth0 --port 49155" << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Set up signal handler
    std::signal(SIGINT, &sig_int_handler);
    
    // Open output file
    std::ofstream outfile(output_file, std::ios::binary);
    if (!outfile.is_open()) {
        std::cerr << "Failed to open output file: " << output_file << std::endl;
        return EXIT_FAILURE;
    }
    
    // Write file header
    struct {
        uint32_t magic;
        uint32_t chdr_width;
        double tick_rate;
    } file_header = {0x43484452, 64, tick_rate};
    outfile.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    
    // Open CSV file if requested
    std::ofstream csv;
    if (!csv_file.empty()) {
        csv.open(csv_file);
        if (!csv.is_open()) {
            std::cerr << "Failed to open CSV file: " << csv_file << std::endl;
            return EXIT_FAILURE;
        }
        
        // Write CSV header
        csv << "packet_num,vc,eob,eov,pkt_type,pkt_type_str,num_mdata,seq_num,length,"
            << "dst_epid,has_timestamp,timestamp_ticks,timestamp_sec,payload_size,"
            << "sample_format,num_samples,first_sample_hex,first_sample_real,first_sample_imag"
            << std::endl;
    }
    
    // Open pcap handle
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* handle = pcap_open_live(interface.c_str(), 65535, 1, 1000, errbuf);
    
    if (!handle) {
        std::cerr << "Failed to open interface " << interface << ": " << errbuf << std::endl;
        std::cerr << "Try running with sudo/administrator privileges" << std::endl;
        return EXIT_FAILURE;
    }
    
    // Build and apply filter
    std::string pcap_filter = "udp port " + std::to_string(port);
    if (!filter.empty()) {
        pcap_filter += " and " + filter;
    }
    
    struct bpf_program fp;
    if (pcap_compile(handle, &fp, pcap_filter.c_str(), 0, PCAP_NETMASK_UNKNOWN) == -1) {
        std::cerr << "Failed to compile filter: " << pcap_geterr(handle) << std::endl;
        pcap_close(handle);
        return EXIT_FAILURE;
    }
    
    if (pcap_setfilter(handle, &fp) == -1) {
        std::cerr << "Failed to set filter: " << pcap_geterr(handle) << std::endl;
        pcap_freecode(&fp);
        pcap_close(handle);
        return EXIT_FAILURE;
    }
    
    std::cout << "Capturing CHDR packets on " << interface << " port " << port << std::endl;
    std::cout << "Filter: " << pcap_filter << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;
    
    // Set up capture context
    struct capture_context {
        std::ofstream* outfile;
        std::ofstream* csv_file;
        size_t* packet_count;
        double tick_rate;
        uint16_t capture_port;
        bool verbose;
    };
    
    size_t packet_count = 0;
    capture_context ctx = {
        &outfile,
        csv_file.empty() ? nullptr : &csv,
        &packet_count,
        tick_rate,
        port,
        verbose
    };
    
    // Start capture
    auto start_time = std::chrono::steady_clock::now();
    
    while (!stop_signal_called && (num_packets == 0 || packet_count < num_packets)) {
        struct pcap_pkthdr* header;
        const u_char* packet;
        
        int res = pcap_next_ex(handle, &header, &packet);
        if (res == 0) continue;  // Timeout
        if (res == -1) {
            std::cerr << "Error reading packet: " << pcap_geterr(handle) << std::endl;
            break;
        }
        if (res == -2) break;  // End of file
        
        packet_handler(reinterpret_cast<u_char*>(&ctx), header, packet);
    }
    
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    double elapsed_secs = std::chrono::duration<double>(elapsed).count();
    
    std::cout << "\n\nCapture complete!" << std::endl;
    std::cout << "Packets captured: " << packet_count << std::endl;
    std::cout << "Duration: " << elapsed_secs << " seconds" << std::endl;
    std::cout << "Packet rate: " << packet_count/elapsed_secs << " packets/sec" << std::endl;
    
    // Cleanup
    pcap_freecode(&fp);
    pcap_close(handle);
    outfile.close();
    if (csv.is_open()) csv.close();
    
    return EXIT_SUCCESS;
}