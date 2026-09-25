// A maintainer's reader of a title's own files, for reverse engineering.
//
// Lists a directory of a disc image's volume, or copies one file out of it,
// through the same core the product mounts titles with: the title keys the
// installation holds decrypt it. What it writes is the player's and belongs
// under the gitignored build/ tree, never in the repository.
//
//   wiiuport_title_files <disc image> <volume directory>
//   wiiuport_title_files <disc image> <volume file> <output file>

#include "wiiuport/shell/HostPaths.h"

#include "Cafe/Filesystem/FST/FST.h"
#include "util/crypto/aes128.h"

#include <lucent/log.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace wiiuport::maint {
namespace {

int listDirectory(FSTVolume& volume, const std::string& directory) {
    FSTDirectoryIterator iterator;
    if (!volume.OpenDirectoryIterator(directory, iterator)) {
        lucent::error("titlefiles", "{} is not a directory of the volume", directory);
        return 1;
    }
    size_t entries = 0;
    FSTFileHandle entry;
    while (volume.Next(iterator, entry)) {
        ++entries;
        if (volume.IsDirectory(entry)) {
            lucent::info("titlefiles", "{}/", volume.GetName(entry));
        } else {
            lucent::info("titlefiles", "{} {}", volume.GetName(entry), volume.GetFileSize(entry));
        }
    }
    lucent::info("titlefiles", "{} entries in {}", entries, directory);
    return 0;
}

int extractFile(FSTVolume& volume, const std::string& file, const std::filesystem::path& output) {
    bool read = false;
    std::vector<uint8> bytes = volume.ExtractFile(file, &read);
    if (!read) {
        lucent::error("titlefiles", "{} is not a file of the volume", file);
        return 1;
    }
    std::filesystem::create_directories(output.parent_path());
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        lucent::error("titlefiles", "could not write {}", output.string());
        return 1;
    }
    lucent::info("titlefiles", "{} ({} bytes) -> {}", file, bytes.size(), output.string());
    return 0;
}

int run(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        lucent::error("titlefiles",
                      "usage: {} <disc image> <volume directory> | <volume file> <output file>",
                      argv[0]);
        return 2;
    }
    shell::HostPaths paths;
    if (!paths.publish(argv[0])) {
        lucent::error("titlefiles", "{}", paths.lastError());
        return 1;
    }
    // The disc is decrypted with the core's AES, whose tables the product's
    // bring-up builds; without them no key decrypts the disc's header.
    AES128_init();
    FSTVolume::ErrorCode failure = FSTVolume::ErrorCode::OK;
    std::unique_ptr<FSTVolume> volume(FSTVolume::OpenFromDiscImage(argv[1], &failure));
    if (!volume) {
        lucent::error("titlefiles", "{} would not open (error {}); are its keys in keys.txt?",
                      argv[1], static_cast<int>(failure));
        return 1;
    }
    return argc == 3 ? listDirectory(*volume, argv[2]) : extractFile(*volume, argv[2], argv[3]);
}

} // namespace
} // namespace wiiuport::maint

int main(int argc, char* argv[]) {
    return wiiuport::maint::run(argc, argv);
}
