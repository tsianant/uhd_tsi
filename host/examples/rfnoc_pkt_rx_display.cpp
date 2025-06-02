//
// Copyright 2014-2016 Ettus Research LLC
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/exception.hpp>
#include <uhd/rfnoc/ddc_block_control.hpp>
#include <uhd/rfnoc/defaults.hpp>
#include <uhd/rfnoc/mb_controller.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/rfnoc_graph.hpp>
#include <uhd/types/sensors.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/utils/graph_utils.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <complex>
#include <csignal>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace po = boost::program_options;

constexpr int64_t UPDATE_INTERVAL = 1; // 1 second update interval for BW summary

static bool stop_signal_called = false;
void sig_int_handler(int)
{
    stop_signal_called = true;
}

extern bool stop_signal_called;


// Structure to store metadata alongside samples
struct packet_metadata_t {
    uint64_t time_spec_full_secs;
    double time_spec_frac_secs;
    uint32_t error_code;
    bool has_time_spec;
    bool start_of_burst;
    bool end_of_burst;
    bool more_fragments;
    uint64_t fragment_offset;
    size_t num_samples;
    uint64_t packet_count;
    
    // Convert to/from binary for file storage
    void to_binary(std::ofstream& file) const {
        file.write(reinterpret_cast<const char*>(this), sizeof(packet_metadata_t));
    }
    
    void from_binary(std::ifstream& file) {
        file.read(reinterpret_cast<char*>(this), sizeof(packet_metadata_t));
    }
};

class UHDMetadataParser {
private:
    std::string samples_filename;
    std::string metadata_filename;
    
public:
    UHDMetadataParser(const std::string& base_filename) 
        : samples_filename(base_filename + "_samples.dat")
        , metadata_filename(base_filename + "_metadata.dat") {}
    
    // Convert UHD metadata to our storage format
    packet_metadata_t convert_uhd_metadata(const uhd::rx_metadata_t& md, 
                                         size_t num_samples, 
                                         uint64_t packet_count) {
        packet_metadata_t pkt_md;
        
        pkt_md.time_spec_full_secs = md.time_spec.get_full_secs();
        pkt_md.time_spec_frac_secs = md.time_spec.get_frac_secs();
        pkt_md.error_code = static_cast<uint32_t>(md.error_code);
        pkt_md.has_time_spec = md.has_time_spec;
        pkt_md.start_of_burst = md.start_of_burst;
        pkt_md.end_of_burst = md.end_of_burst;
        pkt_md.more_fragments = md.more_fragments;
        pkt_md.fragment_offset = md.fragment_offset;
        pkt_md.num_samples = num_samples;
        pkt_md.packet_count = packet_count;
        
        return pkt_md;
    }
    
    // Enhanced receive function that stores both samples and metadata
    template <typename samp_type>
    void recv_to_file_with_metadata(uhd::rx_streamer::sptr rx_stream,
        const size_t samps_per_buff,
        const double rx_rate,
        const unsigned long long num_requested_samples,
        double time_requested = 0.0,
        bool bw_summary = false,
        bool stats = false,
        bool enable_size_map = false,
        bool continue_on_bad_packet = false) {
        
        unsigned long long num_total_samps = 0;
        uint64_t packet_count = 0;
        
        uhd::rx_metadata_t md;
        std::vector<samp_type> buff(samps_per_buff);
        
        // Open files for samples and metadata
        std::ofstream samples_file(samples_filename, std::ofstream::binary);
        std::ofstream metadata_file(metadata_filename, std::ofstream::binary);
        
        if (!samples_file.is_open() || !metadata_file.is_open()) {
            throw std::runtime_error("Failed to open output files");
        }
        
        bool overflow_message = true;
        
        // Setup streaming
        uhd::stream_cmd_t stream_cmd((num_requested_samples == 0)
                                       ? uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS
                                       : uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
        stream_cmd.num_samps = size_t(num_requested_samples);
        stream_cmd.stream_now = true;
        stream_cmd.time_spec = uhd::time_spec_t();
        
        std::cout << "Issuing stream cmd (with metadata capture)" << std::endl;
        rx_stream->issue_stream_cmd(stream_cmd);
        
        const auto start_time = std::chrono::steady_clock::now();
        const auto stop_time = start_time + std::chrono::milliseconds(int64_t(1000 * time_requested));
        auto last_update = start_time;
        unsigned long long last_update_samps = 0;
        
        // Main receive loop
        while (!stop_signal_called &&
               (num_requested_samples != num_total_samps || num_requested_samples == 0) &&
               (time_requested == 0.0 || std::chrono::steady_clock::now() <= stop_time)) {
            
            const auto now = std::chrono::steady_clock::now();
            
            size_t num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md, 3.0, enable_size_map);
            
            // Handle various error conditions
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                std::cout << "Timeout while streaming" << std::endl;
                break;
            }
            
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                if (overflow_message) {
                    overflow_message = false;
                    std::cerr << "Got an overflow indication. Please consider the following:\n"
                             << "  Your write medium must sustain a rate of "
                             << (rx_rate * sizeof(samp_type) / 1e6) << "MB/s.\n"
                             << "  Dropped samples will not be written to the file.\n"
                             << "  This message will not appear again.\n";
                }
                // Still capture metadata even for overflow packets
            }
            
            if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE &&
                md.error_code != uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                std::string error = std::string("Receiver error: ") + md.strerror();
                if (continue_on_bad_packet) {
                    std::cerr << error << std::endl;
                } else {
                    throw std::runtime_error(error);
                }
            }
            
            // Convert and store metadata
            packet_metadata_t pkt_md = convert_uhd_metadata(md, num_rx_samps, packet_count++);
            pkt_md.to_binary(metadata_file);
            
            // Store samples (only if no overflow or if we want to track all packets)
            if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                samples_file.write(reinterpret_cast<const char*>(&buff.front()), 
                                 num_rx_samps * sizeof(samp_type));
                num_total_samps += num_rx_samps;
            }
            
            // Progress reporting
            if (bw_summary && md.error_code != uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                last_update_samps += num_rx_samps;
                const auto time_since_last_update = now - last_update;
                if (time_since_last_update > std::chrono::seconds(1)) {
                    const double time_since_last_update_s =
                        std::chrono::duration<double>(time_since_last_update).count();
                    const double rate = double(last_update_samps) / time_since_last_update_s;
                    std::cout << "\t" << (rate / 1e6) << " MSps" << std::endl;
                    last_update_samps = 0;
                    last_update = now;
                }
            }
        }
        
        // Stop streaming
        stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
        std::cout << "Issuing stop stream cmd" << std::endl;
        rx_stream->issue_stream_cmd(stream_cmd);
        
        // Flush remaining samples
        int num_post_samps = 0;
        do {
            num_post_samps = rx_stream->recv(&buff.front(), buff.size(), md, 3.0);
            if (num_post_samps && md.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE) {
                packet_metadata_t pkt_md = convert_uhd_metadata(md, num_post_samps, packet_count++);
                pkt_md.to_binary(metadata_file);
                samples_file.write(reinterpret_cast<const char*>(&buff.front()), 
                                 num_post_samps * sizeof(samp_type));
            }
        } while (num_post_samps && md.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE);
        
        samples_file.close();
        metadata_file.close();
        
        std::cout << "Captured " << packet_count << " packets with metadata" << std::endl;
    }
    
    // Parse stored metadata and display packet information
    void parse_and_display_metadata() {
        std::ifstream metadata_file(metadata_filename, std::ifstream::binary);
        if (!metadata_file.is_open()) {
            throw std::runtime_error("Failed to open metadata file: " + metadata_filename);
        }
        
        std::cout << "\n=== PACKET METADATA ANALYSIS ===" << std::endl;
        std::cout << std::fixed << std::setprecision(9);
        
        packet_metadata_t pkt_md;
        uint64_t total_packets = 0;
        uint64_t error_packets = 0;
        uint64_t overflow_packets = 0;
        uint64_t total_samples = 0;
        
        // Table header
        std::cout << std::setw(8) << "Packet#" 
                  << std::setw(15) << "Timestamp" 
                  << std::setw(8) << "Samples"
                  << std::setw(8) << "Error" 
                  << std::setw(6) << "SOB" 
                  << std::setw(6) << "EOB"
                  << std::setw(6) << "More"
                  << std::setw(12) << "FragOffset" << std::endl;
        std::cout << std::string(80, '-') << std::endl;
        
        while (metadata_file.read(reinterpret_cast<char*>(&pkt_md), sizeof(packet_metadata_t))) {
            total_packets++;
            total_samples += pkt_md.num_samples;
            
            // Count error types
            if (pkt_md.error_code != 0) {
                error_packets++;
                if (pkt_md.error_code == 1) { // ERROR_CODE_OVERFLOW
                    overflow_packets++;
                }
            }
            
            // Display first 20 packets and any error packets
            if (total_packets <= 20 || pkt_md.error_code != 0) {
                double timestamp = pkt_md.time_spec_full_secs + pkt_md.time_spec_frac_secs;
                
                std::cout << std::setw(8) << pkt_md.packet_count
                          << std::setw(15) << timestamp
                          << std::setw(8) << pkt_md.num_samples
                          << std::setw(8) << pkt_md.error_code
                          << std::setw(6) << (pkt_md.start_of_burst ? "Y" : "N")
                          << std::setw(6) << (pkt_md.end_of_burst ? "Y" : "N")
                          << std::setw(6) << (pkt_md.more_fragments ? "Y" : "N")
                          << std::setw(12) << pkt_md.fragment_offset << std::endl;
            }
        }
        
        metadata_file.close();
        
        // Summary statistics
        std::cout << "\n=== SUMMARY STATISTICS ===" << std::endl;
        std::cout << "Total packets: " << total_packets << std::endl;
        std::cout << "Total samples: " << total_samples << std::endl;
        std::cout << "Error packets: " << error_packets << std::endl;
        std::cout << "Overflow packets: " << overflow_packets << std::endl;
        std::cout << "Success rate: " << std::fixed << std::setprecision(2) 
                  << (100.0 * (total_packets - error_packets) / total_packets) << "%" << std::endl;
        
        if (total_packets > 20) {
            std::cout << "\n(Showing first 20 packets and any error packets)" << std::endl;
        }
    }
    
    // Get metadata for a specific packet
    packet_metadata_t get_packet_metadata(uint64_t packet_index) {
        std::ifstream metadata_file(metadata_filename, std::ifstream::binary);
        if (!metadata_file.is_open()) {
            throw std::runtime_error("Failed to open metadata file");
        }
        
        metadata_file.seekg(packet_index * sizeof(packet_metadata_t));
        packet_metadata_t pkt_md;
        
        if (!metadata_file.read(reinterpret_cast<char*>(&pkt_md), sizeof(packet_metadata_t))) {
            throw std::runtime_error("Failed to read packet metadata at index " + std::to_string(packet_index));
        }
        
        return pkt_md;
    }
    
    // Print detailed information about UHD metadata fields
    void print_metadata_field_descriptions() {
        std::cout << "\n=== UHD METADATA FIELD DESCRIPTIONS ===" << std::endl;
        std::cout << "time_spec_full_secs: Full seconds portion of timestamp" << std::endl;
        std::cout << "time_spec_frac_secs: Fractional seconds portion of timestamp" << std::endl;
        std::cout << "error_code: Error status (0=none, 1=overflow, 2=timeout, 3=bad_packet, etc.)" << std::endl;
        std::cout << "has_time_spec: Whether packet includes timestamp information" << std::endl;
        std::cout << "start_of_burst: Indicates first packet in a burst" << std::endl;
        std::cout << "end_of_burst: Indicates last packet in a burst" << std::endl;
        std::cout << "more_fragments: Indicates packet fragmentation" << std::endl;
        std::cout << "fragment_offset: Offset for fragmented packets" << std::endl;
        std::cout << "num_samples: Number of samples in this packet" << std::endl;
        std::cout << "packet_count: Sequential packet number" << std::endl;
    }
};

// External variables (would be defined in your main file)
extern bool stop_signal_called;

// Usage example function to integrate with your existing code
void example_usage_with_metadata() {
    // Create parser instance
    UHDMetadataParser parser("usrp_capture");
    
    // Print field descriptions
    parser.print_metadata_field_descriptions();
    
    // Example of using the enhanced receive function
    // (This would replace your existing recv_to_file call)
    /*
    if (format == "fc64")
        parser.recv_to_file_with_metadata<std::complex<double>>(
            rx_stream, spb, rate, total_num_samps, total_time, 
            bw_summary, stats, enable_size_map, continue_on_bad_packet);
    else if (format == "fc32")
        parser.recv_to_file_with_metadata<std::complex<float>>(
            rx_stream, spb, rate, total_num_samps, total_time,
            bw_summary, stats, enable_size_map, continue_on_bad_packet);
    else if (format == "sc16")
        parser.recv_to_file_with_metadata<std::complex<short>>(
            rx_stream, spb, rate, total_num_samps, total_time,
            bw_summary, stats, enable_size_map, continue_on_bad_packet);
    */
    
    // After capture, parse and display the metadata
    parser.parse_and_display_metadata();
    
    // Example: Get specific packet metadata
    try {
        auto packet_0_md = parser.get_packet_metadata(0);
        std::cout << "First packet had " << packet_0_md.num_samples << " samples" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Error reading packet metadata: " << e.what() << std::endl;
    }
}









template <typename samp_type>
void recv_to_file(uhd::rx_streamer::sptr rx_stream,
    const std::string& file,
    const size_t samps_per_buff,
    const double rx_rate,
    const unsigned long long num_requested_samples,
    double time_requested       = 0.0,
    bool bw_summary             = false,
    bool stats                  = false,
    bool enable_size_map        = false,
    bool continue_on_bad_packet = false)
{
    unsigned long long num_total_samps = 0;

    uhd::rx_metadata_t md;
    std::vector<samp_type> buff(samps_per_buff);
    std::ofstream outfile;
    if (not file.empty()) {
        outfile.open(file.c_str(), std::ofstream::binary);
    }
    bool overflow_message = true;

    // setup streaming
    uhd::stream_cmd_t stream_cmd((num_requested_samples == 0)
                                     ? uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS
                                     : uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    stream_cmd.num_samps  = size_t(num_requested_samples);
    stream_cmd.stream_now = true;
    stream_cmd.time_spec  = uhd::time_spec_t();
    std::cout << "Issuing stream cmd" << std::endl;
    rx_stream->issue_stream_cmd(stream_cmd);

    const auto start_time = std::chrono::steady_clock::now();
    const auto stop_time =
        start_time + std::chrono::milliseconds(int64_t(1000 * time_requested));
    // Track time and samps between updating the BW summary
    auto last_update                     = start_time;
    unsigned long long last_update_samps = 0;

    typedef std::map<size_t, size_t> SizeMap;
    SizeMap mapSizes;

    // Run this loop until either time expired (if a duration was given), until
    // the requested number of samples were collected (if such a number was
    // given), or until Ctrl-C was pressed.
    while (not stop_signal_called
           and (num_requested_samples != num_total_samps or num_requested_samples == 0)
           and (time_requested == 0.0 or std::chrono::steady_clock::now() <= stop_time)) {
        const auto now = std::chrono::steady_clock::now();

        size_t num_rx_samps =
            rx_stream->recv(&buff.front(), buff.size(), md, 3.0, enable_size_map);

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << "Timeout while streaming" << std::endl;
            break;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            if (overflow_message) {
                overflow_message = false;
                std::cerr
                    << "Got an overflow indication. Please consider the following:\n"
                       "  Your write medium must sustain a rate of "
                    << (rx_rate * sizeof(samp_type) / 1e6)
                    << "MB/s.\n"
                       "  Dropped samples will not be written to the file.\n"
                       "  Please modify this example for your purposes.\n"
                       "  This message will not appear again.\n";
            }
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::string error = std::string("Receiver error: ") + md.strerror();
            if (continue_on_bad_packet) {
                std::cerr << error << std::endl;
                continue;
            } else
                throw std::runtime_error(error);
        }

        if (enable_size_map) {
            SizeMap::iterator it = mapSizes.find(num_rx_samps);
            if (it == mapSizes.end())
                mapSizes[num_rx_samps] = 0;
            mapSizes[num_rx_samps] += 1;
        }

        num_total_samps += num_rx_samps;

        if (outfile.is_open()) {
            outfile.write((const char*)&buff.front(), num_rx_samps * sizeof(samp_type));
        }

        if (bw_summary) {
            last_update_samps += num_rx_samps;
            const auto time_since_last_update = now - last_update;
            if (time_since_last_update > std::chrono::seconds(UPDATE_INTERVAL)) {
                const double time_since_last_update_s =
                    std::chrono::duration<double>(time_since_last_update).count();
                const double rate = double(last_update_samps) / time_since_last_update_s;
                std::cout << "\t" << (rate / 1e6) << " MSps" << std::endl;
                last_update_samps = 0;
                last_update       = now;
            }
        }
    }
    const auto actual_stop_time = std::chrono::steady_clock::now();

    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    std::cout << "Issuing stop stream cmd" << std::endl;
    rx_stream->issue_stream_cmd(stream_cmd);

    // Run recv until nothing is left
    int num_post_samps = 0;
    do {
        num_post_samps = rx_stream->recv(&buff.front(), buff.size(), md, 3.0);
    } while (num_post_samps and md.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE);

    if (outfile.is_open())
        outfile.close();

    if (stats) {
        std::cout << std::endl;

        const double actual_duration_seconds =
            std::chrono::duration<float>(actual_stop_time - start_time).count();

        std::cout << "Received " << num_total_samps << " samples in "
                  << actual_duration_seconds << " seconds" << std::endl;
        const double rate = (double)num_total_samps / actual_duration_seconds;
        std::cout << (rate / 1e6) << " MSps" << std::endl;

        if (enable_size_map) {
            std::cout << std::endl;
            std::cout << "Packet size map (bytes: count)" << std::endl;
            for (SizeMap::iterator it = mapSizes.begin(); it != mapSizes.end(); it++)
                std::cout << it->first << ":\t" << it->second << std::endl;
        }
    }
}

typedef std::function<uhd::sensor_value_t(const std::string&)> get_sensor_fn_t;

bool check_locked_sensor(std::vector<std::string> sensor_names,
    const char* sensor_name,
    get_sensor_fn_t get_sensor_fn,
    double setup_time)
{
    if (std::find(sensor_names.begin(), sensor_names.end(), sensor_name)
        == sensor_names.end())
        return false;

    auto setup_timeout = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(int64_t(setup_time * 1000));
    bool lock_detected = false;

    std::cout << "Waiting for \"" << sensor_name << "\": ";
    std::cout.flush();

    while (true) {
        if (lock_detected and (std::chrono::steady_clock::now() > setup_timeout)) {
            std::cout << " locked." << std::endl;
            break;
        }
        if (get_sensor_fn(sensor_name).to_bool()) {
            std::cout << "+";
            std::cout.flush();
            lock_detected = true;
        } else {
            if (std::chrono::steady_clock::now() > setup_timeout) {
                std::cout << std::endl;
                throw std::runtime_error(
                    std::string("timed out waiting for consecutive locks on sensor \"")
                    + sensor_name + "\"");
            }
            std::cout << "_";
            std::cout.flush();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << std::endl;
    return true;
}

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // variables to be set by po
    std::string args, file, format, ant, subdev, ref, wirefmt, streamargs, block_id,
        block_props;
    size_t total_num_samps, spb, spp, radio_id, radio_chan, block_port;
    double rate, freq, gain, bw, total_time, setup_time, lo_offset;

    // setup the program options
    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help", "help message")
        ("file", po::value<std::string>(&file)->default_value("usrp_samples.dat"), "name of the file to write binary samples to")
        ("format", po::value<std::string>(&format)->default_value("sc16"), "File sample format: sc16, fc32, or fc64")
        ("duration", po::value<double>(&total_time)->default_value(0), "total number of seconds to receive")
        ("nsamps", po::value<size_t>(&total_num_samps)->default_value(0), "total number of samples to receive")
        ("spb", po::value<size_t>(&spb)->default_value(10000), "samples per buffer")
        ("spp", po::value<size_t>(&spp), "samples per packet (on FPGA and wire)")
        ("streamargs", po::value<std::string>(&streamargs)->default_value(""), "stream args")
        ("progress", "periodically display short-term bandwidth")
        ("stats", "show average bandwidth on exit")
        ("sizemap", "track packet size and display breakdown on exit")
        ("null", "run without writing to file")
        ("continue", "don't abort on a bad packet")

        ("args", po::value<std::string>(&args)->default_value(""), "USRP device address args")
        ("setup", po::value<double>(&setup_time)->default_value(1.0), "seconds of setup time")

        ("radio-id", po::value<size_t>(&radio_id)->default_value(0), "Radio ID to use (0 or 1).")
        ("radio-chan", po::value<size_t>(&radio_chan)->default_value(0), "Radio channel")
        ("rate", po::value<double>(&rate)->default_value(1e6), "RX rate of the radio block")
        ("freq", po::value<double>(&freq)->default_value(0.0), "RF center frequency in Hz")
        ("lo-offset", po::value<double>(&lo_offset), "Offset for frontend LO in Hz (optional)")
        ("gain", po::value<double>(&gain), "gain for the RF chain")
        ("ant", po::value<std::string>(&ant), "antenna selection")
        ("bw", po::value<double>(&bw), "analog frontend filter bandwidth in Hz")
        ("ref", po::value<std::string>(&ref), "reference source (internal, external, mimo)")
        ("skip-lo", "skip checking LO lock status")
        ("int-n", "tune USRP with integer-N tuning")

        ("block-id", po::value<std::string>(&block_id), "If block ID is specified, this block is inserted between radio and host.")
        ("block-port", po::value<size_t>(&block_port)->default_value(0), "If block ID is specified, this block is inserted between radio and host.")
        ("block-props", po::value<std::string>(&block_props), "These are passed straight to the block as properties (see set_properties()).")

        ("metadata-only", "parse existing metadata file without capturing")
        ("metadata-file", po::value<std::string>(), "base filename for metadata analysis")
    ;
    // clang-format on
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // print the help message
    if (vm.count("help")) {
        std::cout << "UHD/RFNoC RX samples to file " << desc << std::endl;
        std::cout << std::endl
                  << "This application streams data from a single channel of a USRP "
                     "device to a file.\n"
                  << std::endl;
        return ~0;
    }

    bool bw_summary = vm.count("progress") > 0;
    bool stats      = vm.count("stats") > 0;
    if (vm.count("null") > 0) {
        file = "";
    }
    bool enable_size_map        = vm.count("sizemap") > 0;
    bool continue_on_bad_packet = vm.count("continue") > 0;

    if (enable_size_map) {
        std::cout << "Packet size tracking enabled - will only recv one packet at a time!"
                  << std::endl;
    }

    if (format != "sc16" and format != "fc32" and format != "fc64") {
        std::cout << "Invalid sample format: " << format << std::endl;
        return EXIT_FAILURE;
    }

    /************************************************************************
     * Create device and block controls
     ***********************************************************************/
    std::cout << std::endl;
    std::cout << "Creating the RFNoC graph with args: " << args << std::endl;
    auto graph = uhd::rfnoc::rfnoc_graph::make(args);

    // Create handle for radio object
    uhd::rfnoc::block_id_t radio_ctrl_id(0, "Radio", radio_id);
    // This next line will fail if the radio is not actually available
    auto radio_ctrl = graph->get_block<uhd::rfnoc::radio_control>(radio_ctrl_id);
    std::cout << "Using radio " << radio_id << ", channel " << radio_chan << std::endl;

    uhd::rfnoc::block_id_t last_block_in_chain;
    size_t last_port_in_chain;
    uhd::rfnoc::ddc_block_control::sptr ddc_ctrl;
    size_t ddc_chan       = 0;
    bool user_block_found = false;

    { // First, connect everything dangling off of the radio
        auto edges = uhd::rfnoc::get_block_chain(graph, radio_ctrl_id, radio_chan, true);
        last_block_in_chain = edges.back().src_blockid;
        last_port_in_chain  = edges.back().src_port;
        if (edges.size() > 1) {
            uhd::rfnoc::connect_through_blocks(graph,
                radio_ctrl_id,
                radio_chan,
                last_block_in_chain,
                last_port_in_chain);
            for (auto& edge : edges) {
                if (uhd::rfnoc::block_id_t(edge.dst_blockid).get_block_name() == "DDC") {
                    ddc_ctrl =
                        graph->get_block<uhd::rfnoc::ddc_block_control>(edge.dst_blockid);
                    ddc_chan = edge.dst_port;
                }
                if (vm.count("block-id") && edge.dst_blockid == block_id) {
                    user_block_found = true;
                }
            }
        }
    }

    // If the user block is not in the chain yet, see if we can connect that
    // separately
    if (vm.count("block-id") && !user_block_found) {
        const auto user_block_id = uhd::rfnoc::block_id_t(block_id);
        if (!graph->has_block(user_block_id)) {
            std::cout << "ERROR! No such block: " << block_id << std::endl;
            return EXIT_FAILURE;
        }
        std::cout << "Attempting to connect " << block_id << ":" << last_port_in_chain
                  << " to " << last_block_in_chain << ":" << block_port << "..."
                  << std::endl;
        uhd::rfnoc::connect_through_blocks(
            graph, last_block_in_chain, last_port_in_chain, user_block_id, block_port);
        last_block_in_chain = uhd::rfnoc::block_id_t(block_id);
        last_port_in_chain  = block_port;
        // Now we have to make sure that there are no more static connections
        // after the user-defined block
        auto edges = uhd::rfnoc::get_block_chain(
            graph, last_block_in_chain, last_port_in_chain, true);
        if (edges.size() > 1) {
            uhd::rfnoc::connect_through_blocks(graph,
                last_block_in_chain,
                last_port_in_chain,
                edges.back().src_blockid,
                edges.back().src_port);
            last_block_in_chain = edges.back().src_blockid;
            last_port_in_chain  = edges.back().src_port;
        }
    }
    /************************************************************************
     * Set up radio
     ***********************************************************************/
    // Lock mboard clocks
    if (vm.count("ref")) {
        graph->get_mb_controller(0)->set_clock_source(ref);
    }

    // set the rf gain
    if (vm.count("gain")) {
        std::cout << "Requesting RX Gain: " << gain << " dB..." << std::endl;
        radio_ctrl->set_rx_gain(gain, radio_chan);
        std::cout << "Actual RX Gain: " << radio_ctrl->get_rx_gain(radio_chan) << " dB..."
                  << std::endl
                  << std::endl;
    }

    // set the IF filter bandwidth
    if (vm.count("bw")) {
        std::cout << "Requesting RX Bandwidth: " << (bw / 1e6) << " MHz..." << std::endl;
        radio_ctrl->set_rx_bandwidth(bw, radio_chan);
        std::cout << "Actual RX Bandwidth: "
                  << (radio_ctrl->get_rx_bandwidth(radio_chan) / 1e6) << " MHz..."
                  << std::endl
                  << std::endl;
    }

    // set the antenna
    if (vm.count("ant")) {
        radio_ctrl->set_rx_antenna(ant, radio_chan);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(1000 * setup_time)));

    // check Ref and LO Lock detect
    if (not vm.count("skip-lo")) {
        check_locked_sensor(
            radio_ctrl->get_rx_sensor_names(radio_chan),
            "lo_locked",
            [&](const std::string& sensor_name) {
                return radio_ctrl->get_rx_sensor(sensor_name, radio_chan);
            },
            setup_time);
        if (ref == "external") {
            check_locked_sensor(
                graph->get_mb_controller(0)->get_sensor_names(),
                "ref_locked",
                [&](const std::string& sensor_name) {
                    return graph->get_mb_controller(0)->get_sensor(sensor_name);
                },
                setup_time);
        }
    }

    if (vm.count("spp")) {
        std::cout << "Requesting samples per packet of: " << spp << std::endl;
        radio_ctrl->set_property<int>("spp", spp, radio_chan);
        spp = radio_ctrl->get_property<int>("spp", radio_chan);
        std::cout << "Actual samples per packet = " << spp << std::endl;
    }

    /************************************************************************
     * Set up streaming
     ***********************************************************************/
    uhd::device_addr_t streamer_args(streamargs);

    // create a receive streamer
    uhd::stream_args_t stream_args(
        format, "sc16"); // We should read the wire format from the blocks
    stream_args.args = streamer_args;
    std::cout << "Using streamer args: " << stream_args.args.to_string() << std::endl;
    auto rx_stream = graph->create_rx_streamer(1, stream_args);

    // Connect streamer to last block and commit the graph
    graph->connect(last_block_in_chain, last_port_in_chain, rx_stream, 0);
    graph->commit();
    std::cout << "Active connections:" << std::endl;
    for (auto& edge : graph->enumerate_active_connections()) {
        std::cout << "* " << edge.to_string() << std::endl;
    }

    /************************************************************************
     * Set up sampling rate and (optional) user block properties. We do this
     * after commit() so we can use the property propagation.
     ***********************************************************************/

    // set the center frequency
    if (vm.count("freq")) {
        std::cout << "Requesting RX Freq: " << (freq / 1e6) << " MHz..." << std::endl;
        uhd::tune_request_t tune_request(freq);

        if (vm.count("lo-offset")) {
            std::cout << boost::format("Setting RX LO Offset: %f MHz...")
                             % (lo_offset / 1e6)
                      << std::endl;
            tune_request = uhd::tune_request_t(freq, lo_offset);
        }

        if (vm.count("int-n")) {
            tune_request.args = uhd::device_addr_t("mode_n=integer");
        }
        auto tune_req_action = uhd::rfnoc::tune_request_action_info::make(tune_request);
        tune_req_action->tune_request = tune_request;
        rx_stream->post_input_action(tune_req_action, 0);

        std::cout << "Actual RX Freq: "
                  << (radio_ctrl->get_rx_frequency(radio_chan) / 1e6) << " MHz..."
                  << std::endl
                  << std::endl;
    }

    // set the sample rate
    if (rate <= 0.0) {
        std::cerr << "Please specify a valid sample rate" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Requesting RX Rate: " << (rate / 1e6) << " Msps..." << std::endl;
    if (ddc_ctrl) {
        std::cout << "Setting rate on DDC block!" << std::endl;
        rate = ddc_ctrl->set_output_rate(rate, ddc_chan);
    } else {
        std::cout << "Setting rate on radio block!" << std::endl;
        rate = radio_ctrl->set_rate(rate);
    }
    std::cout << "Actual RX Rate: " << (rate / 1e6) << " Msps..." << std::endl
              << std::endl;

    if (vm.count("block-props")) {
        std::cout << "Setting block properties to: " << block_props << std::endl;
        graph->get_block(uhd::rfnoc::block_id_t(block_id))
            ->set_properties(uhd::device_addr_t(block_props));
    }

    if (total_num_samps == 0) {
        std::signal(SIGINT, &sig_int_handler);
        std::cout << "Press Ctrl + C to stop streaming..." << std::endl;
    }


// #define recv_to_file_args() \
//     (rx_stream,             \
//         file,               \
//         spb,                \
//         rate,               \
//         total_num_samps,    \
//         total_time,         \
//         bw_summary,         \
//         stats,              \
//         enable_size_map,    \
//         continue_on_bad_packet)


    // recv to file
    // if (format == "fc64")
    //     recv_to_file<std::complex<double>> recv_to_file_args();
    // else if (format == "fc32")
    //     recv_to_file<std::complex<float>> recv_to_file_args();
    // else if (format == "sc16")
    //     recv_to_file<std::complex<short>> recv_to_file_args();
    // else
    //     throw std::runtime_error("Unknown data format: " + format);




    UHDMetadataParser parser(file.empty() ? "usrp_capture" : file);

    if (format == "fc64")
        parser.recv_to_file_with_metadata<std::complex<double>>(
            rx_stream, spb, rate, total_num_samps, total_time,
            bw_summary, stats, enable_size_map, continue_on_bad_packet);
    else if (format == "fc32")
        parser.recv_to_file_with_metadata<std::complex<float>>(
            rx_stream, spb, rate, total_num_samps, total_time,
            bw_summary, stats, enable_size_map, continue_on_bad_packet);
    else if (format == "sc16")
        parser.recv_to_file_with_metadata<std::complex<short>>(
            rx_stream, spb, rate, total_num_samps, total_time,
            bw_summary, stats, enable_size_map, continue_on_bad_packet);

    // After capture, analyze the metadata
    parser.parse_and_display_metadata();





    // finished
    std::cout << std::endl << "Done!" << std::endl << std::endl;

    return EXIT_SUCCESS;
}
