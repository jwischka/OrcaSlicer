#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/SnapmakerProtocol.hpp"

using namespace Slic3r;

TEST_CASE("U1 loaded-spool selections reach the native print mapping without changing slice tool IDs", "[Snapmaker][ToolMapping]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_map", new ConfigOptionInts{1, 2, 3, 4});
    config.set_key_value("filament_device_map", new ConfigOptionInts{0, 3, 0, 0});
    const auto mapping = SnapmakerProtocol::filament_mapping(config);
    CHECK(mapping == "[0,3,0,0]");
    CHECK(SnapmakerProtocol::start_script("sliding capy.gcode", mapping) ==
          "SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"sliding capy.gcode\" MAP_TABLE=\"[[0,0],[1,2],[2,2],[3,3]]\"");
    CHECK(config.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{1, 2, 3, 4});

    DynamicPrintConfig restored;
    restored.set_deserialize_strict("filament_device_map", config.option("filament_device_map")->serialize());
    CHECK(SnapmakerProtocol::filament_mapping(restored) == mapping);
}

TEST_CASE("U1 jobs without loaded-spool assignments keep their existing start behavior", "[Snapmaker][ToolMapping]")
{
    DynamicPrintConfig config;
    CHECK(SnapmakerProtocol::filament_mapping(config).empty());
    config.set_key_value("filament_device_map", new ConfigOptionInts{0, 0, 0, 0});
    CHECK(SnapmakerProtocol::filament_mapping(config).empty());
}

TEST_CASE("Invalid U1 mappings cannot generate a print start command", "[Snapmaker][ToolMapping]")
{
    for (const auto* mapping : {"", "[]", "[0,5]", "[-1,3]", "[0,2.5]", "[0,\"3\"]", "{}"}) {
        CAPTURE(mapping);
        CHECK(SnapmakerProtocol::start_script("part.gcode", mapping).empty());
    }
    CHECK(SnapmakerProtocol::start_script("part\"\nG28", "[0,3]").empty());
}
