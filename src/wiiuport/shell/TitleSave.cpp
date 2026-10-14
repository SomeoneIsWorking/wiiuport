#include "wiiuport/shell/TitleSave.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/ActiveSettings.h"

#include <format>
#include <string>

namespace wiiuport::shell {

std::optional<std::vector<uint8_t>> TitleSave::read(std::string_view file) {
    const TitleId title = CafeSystem::GetForegroundTitleId();
    const std::string path =
        std::format("/vol/storage_mlc01/usr/save/{:08x}/{:08x}/user/{:08x}/{}",
                    static_cast<uint32_t>(title >> 32), static_cast<uint32_t>(title),
                    ActiveSettings::GetPersistentId(), file);
    return fsc_extractFile(path.c_str());
}

} // namespace wiiuport::shell
