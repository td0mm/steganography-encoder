#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>

#include "image.hpp"
#include "utils.hpp"
#include "random.hpp"

#define VERSION 1
#define LEVEL Image::EncodingLevel::Low

namespace fs = std::filesystem;

struct Header {
    std::uint8_t  sig[4];
    std::uint16_t version;
    std::uint8_t  level;
    std::uint8_t  flags;
    std::uint32_t offset;
    std::uint32_t size;
    std::uint8_t  name[32];
    std::uint8_t  reserved[12];
};

const char* level_to_str[3] = {
    "Low (Default)",
    "Medium",
    "High"
};

int encode(Image& image, const std::string& input, const std::string& output, Image::EncodingLevel level) {
    std::ifstream file(input, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "ERROR: Unable to open file '" << input << "'" << std::endl;
        return -1;
    }

    std::cout << "* Image size: " << image.w() << "x" << image.h() << " pixels" << std::endl;
    std::cout << "* Encoding level: " << level_to_str[static_cast<int>(level)] << std::endl;

    std::size_t size = file.tellg();
    std::size_t padded_size = size + 1;
    if (padded_size % 16)
        padded_size = (size / 16 + 1) * 16;

    unsigned int max_size = image.w() * image.h() * 4 / Image::encoded_size(1, level) - Image::encoded_size(sizeof(Header), Image::EncodingLevel::Low);

    std::cout << "* Max embed size: " << data_size(max_size) << std::endl;
    std::cout << "* Embed size: " << data_size(size) << std::endl;

    if (padded_size > max_size) {
        std::cerr << "ERROR: Data-File too big, maximum possible size: " << (max_size / 1024) << " KiB" << std::endl;
        return -1;
    }

    auto padded_data = std::make_unique<uint8_t[]>(padded_size);
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(padded_data.get()), size);
    file.close();

    std::uint8_t left = padded_size - size;
    std::fill_n(padded_data.get() + size, left, left);

    std::uint32_t offset;
    Random random;
    if (!random.get(&offset, sizeof(offset))) {
        std::cerr << "Unable to generate random number" << std::endl;
        return -1;
    }

    offset = (offset + Image::encoded_size(sizeof(Header), Image::EncodingLevel::Low)) % (Image::encoded_size(max_size - padded_size, level));

    Header header{};
    header.sig[0] = 'H'; header.sig[1] = 'I'; header.sig[2] = 'D'; header.sig[3] = 'E';
    header.version = VERSION;
    header.level = static_cast<std::uint8_t>(level);
    header.flags = 0;
    header.offset = offset;
    header.size = padded_size;

    auto name = fs::path(input).filename().string();
    if (name.size() > sizeof(header.name)) {
        std::cerr << "ERROR: File name '" << name << "' is over 32 characters" << std::endl;
        return -1;
    }
    std::copy_n(name.data(), name.size(), header.name);
    std::fill_n(&header.name[name.size()], sizeof(header.name) - name.size(), 0x00);
    std::fill_n(header.reserved, sizeof(header.reserved), 0x00);

    image.encode(reinterpret_cast<const uint8_t*>(&header), sizeof(header), level);
    image.encode(padded_data.get(), padded_size, level, offset);

    std::cout << "* Embedded " << name << " into image" << std::endl;

    if (!image.save(output)) {
        std::cout << "Unable to save image!" << std::endl;
        return false;
    }

    std::cout << "* Successfully wrote to " << output << std::endl;
    return true;
}

int decode(Image& image, std::string output) {
    std::cout << "* Image size: " << image.w() << "x" << image.h() << " pixels" << std::endl;

    auto header_data = image.decode(sizeof(Header), Image::EncodingLevel::Low);
    Header header;
    std::memcpy(&header, header_data.get(), sizeof(Header));

    if (header.sig[0] != 'H' || header.sig[1] != 'I' || header.sig[2] != 'D' || header.sig[3] != 'E') {
        std::cerr << "ERROR: Invalid header signature" << std::endl;
        return -1;
    }

    if (header.version != VERSION) {
        std::cerr << "ERROR: Unsupported file-version " << header.version << std::endl;
        return -1;
    }

    for (auto r : header.reserved) {
        if (r) {
            std::cerr << "ERROR: Invalid reserved bytes" << std::endl;
            return -1;
        }
    }

    std::string name;
    if (header.name[sizeof(header.name) - 1])
        name = std::string(reinterpret_cast<char*>(header.name), sizeof(header.name));
    else
        name = std::string(reinterpret_cast<char*>(header.name));

    std::cout << "* Detected embed " << name << std::endl;
    std::cout << "* Encoding level: " << level_to_str[header.level] << std::endl;

    auto data = image.decode(header.size, static_cast<Image::EncodingLevel>(header.level), header.offset);

    std::uint8_t left = data[header.size - 1];
    std::size_t size = header.size - left;

    if (output.empty())
        output = name;

    std::ofstream file(output, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "ERROR: Unable to save file '" << output << "'" << std::endl;
        return -1;
    }

    file.write(reinterpret_cast<char*>(data.get()), size);
    file.close();

    std::cout << "* Successfully wrote to " << output << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage:\n  steganography encode -i inputfile -e embedfile -o outputfile\n  steganography decode -i inputfile -o outputfile" << std::endl;
        return -1;
    }

    std::string mode = argv[1];

    if (mode == "encode") {
        std::string input_img, embed_file, output_img;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "-i" || arg == "--input") && i + 1 < argc) input_img = argv[++i];
            else if ((arg == "-e" || arg == "--embed") && i + 1 < argc) embed_file = argv[++i];
            else if ((arg == "-o" || arg == "--output") && i + 1 < argc) output_img = argv[++i];
        }
        if (input_img.empty() || embed_file.empty() || output_img.empty()) {
            std::cerr << "Usage: steganography encode -i inputfile -e embedfile -o outputfile" << std::endl;
            return -1;
        }
        Image image;
        if (!image.load(input_img)) {
            std::cerr << "ERROR: Failed to load image " << input_img << std::endl;
            return -1;
        }
        return encode(image, embed_file, output_img, LEVEL);
    }
    else if (mode == "decode") {
        std::string input_img, output_file;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "-i" || arg == "--input") && i + 1 < argc) input_img = argv[++i];
            else if ((arg == "-o" || arg == "--output") && i + 1 < argc) output_file = argv[++i];
        }
        if (input_img.empty() || output_file.empty()) {
            std::cerr << "Usage: steganography decode -i inputfile -o outputfile" << std::endl;
            return -1;
        }
        Image image;
        if (!image.load(input_img)) {
            std::cerr << "ERROR: Failed to load image " << input_img << std::endl;
            return -1;
        }
        return decode(image, output_file);
    }
    else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        return -1;
    }
}
