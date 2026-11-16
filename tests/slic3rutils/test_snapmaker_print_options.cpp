#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <initializer_list>
#include <string>
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/SnapmakerProtocol.hpp"

#include <nlohmann/json.hpp>

#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

using namespace Slic3r;
using nlohmann::json;

namespace {

PrintParams make_params(bool bed_leveling, bool flow_cali, bool timelapse, const std::string& ams_mapping, int plate_index = 1)
{
    PrintParams params;
    params.task_bed_leveling     = bed_leveling;
    params.task_flow_cali        = flow_cali;
    params.task_record_timelapse = timelapse;
    params.ams_mapping           = ams_mapping;
    params.plate_index           = plate_index;
    return params;
}

} // namespace

TEST_CASE("Firmware from 1.3.0 on starts prints with options", "[SnapmakerPrinterAgent]")
{
    const std::string version = GENERATE(as<std::string>{}, "1.3.0", "1.3", "v1.3.0", "1.3.1", "1.10.0", "2.0.0", "1.3.0-beta.2",
                                         "1.3.0.1234", " 1.4.2\n");
    CAPTURE(version);
    CHECK(SnapmakerPrinterAgent::firmware_supports_print_options(version));
}

TEST_CASE("Older or unreadable firmware versions start prints without options", "[SnapmakerPrinterAgent]")
{
    const std::string version = GENERATE(as<std::string>{}, "", "?", "1.2.9", "1.2", "0.9.0", "v1.0.3", "t1.3.0-g1234abc-shallow",
                                         "latest", "99999999999.0.0");
    CAPTURE(version);
    CHECK_FALSE(SnapmakerPrinterAgent::firmware_supports_print_options(version));
}

TEST_CASE("Loaded spool selections become zero-based firmware pairs", "[SnapmakerPrinterAgent][ToolMapping]")
{
    CHECK(SnapmakerPrinterAgent::build_map_table("[3, 1, 2, 4]") == json::parse("[[0, 2], [1, 0], [2, 1], [3, 3]]"));
    // Unassigned slots retain their default physical head, while a virtual slot needs an assignment.
    CHECK(SnapmakerPrinterAgent::build_map_table("[0, 3, 0, 0, 0, 2]") ==
          json::parse("[[0, 0], [1, 2], [2, 2], [3, 3], [5, 1]]"));
}

TEST_CASE("Malformed loaded spool selections cannot produce a firmware mapping", "[SnapmakerPrinterAgent][ToolMapping]")
{
    const std::string mapping = GENERATE(as<std::string>{}, "", "[]", "[-1, 3]", "[5]", "[0, 2.5]", "[0, \"3\"]", "{\"0\": 1}", "[1,");
    CAPTURE(mapping);
    CHECK(SnapmakerPrinterAgent::build_map_table(mapping).empty());
}

TEST_CASE("The mapping respects the firmware's limit of 32 logical filaments", "[SnapmakerPrinterAgent][ToolMapping]")
{
    json mapping = json::array();
    for (int filament = 0; filament < 31; ++filament)
        mapping.push_back(0);
    mapping.push_back(3);
    const json table = SnapmakerPrinterAgent::build_map_table(mapping.dump());
    REQUIRE(table.size() == 5);
    CHECK(table.back() == json::parse("[31, 2]"));
    mapping.push_back(1);
    CHECK(SnapmakerPrinterAgent::build_map_table(mapping.dump()).empty());
}

TEST_CASE("Prepare selections reach native print options and keep the legacy mapping", "[SnapmakerPrinterAgent][ToolMapping]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_device_map", new ConfigOptionInts{0, 3, 0, 0});
    const PrintParams params = make_params(true, false, true, SnapmakerProtocol::filament_mapping(config));
    const json table = SnapmakerPrinterAgent::build_start_local_print_params(params, "part.gcode")["options"]["map_table"];
    CHECK(table == json::parse("[[0, 0], [1, 2], [2, 2], [3, 3]]"));
    CHECK(SnapmakerProtocol::start_script("part.gcode", params.ams_mapping) ==
          "SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"part.gcode\" MAP_TABLE=\"" + table.dump() + "\"");
}

TEST_CASE("The print dialog's choices become start_local_print options", "[SnapmakerPrinterAgent]")
{
    const json request = SnapmakerPrinterAgent::build_start_local_print_params(make_params(true, false, true, "[2, 0, 1]"),
                                                                               "Clamp Nut.gcode");
    CHECK(request == json::parse(R"({
        "path": "Clamp Nut.gcode",
        "options": {"bed_level": 1, "flow_calibrate": 0, "time_lapse_camera": 1, "map_table": [[0, 1], [1, 1], [2, 0]]}
    })"));
}

TEST_CASE("The switches are sent as 0 or 1, never as JSON booleans", "[SnapmakerPrinterAgent]")
{
    // Moonraker writes each option with Python's str(), and the firmware's get_int() rejects "True".
    const json options = SnapmakerPrinterAgent::build_start_local_print_params(make_params(true, true, true, ""), "a.gcode")["options"];
    for (const char* key : {"bed_level", "flow_calibrate", "time_lapse_camera"}) {
        CAPTURE(key);
        CHECK(options[key].is_number_integer());
        CHECK(options[key] == 1);
    }
}

TEST_CASE("Without a mapping the request leaves the firmware's default map table", "[SnapmakerPrinterAgent]")
{
    const json request = SnapmakerPrinterAgent::build_start_local_print_params(make_params(false, false, false, ""), "a.gcode");
    CHECK_FALSE(request["options"].contains("map_table"));
}

TEST_CASE("Only a 3MF names the plate to print", "[SnapmakerPrinterAgent]")
{
    const PrintParams params = make_params(false, false, false, "", 2);

    CHECK(SnapmakerPrinterAgent::build_start_local_print_params(params, "Cube.gcode.3mf")["print_plate"] == 2);
    CHECK_FALSE(SnapmakerPrinterAgent::build_start_local_print_params(params, "Cube.gcode").contains("print_plate"));
}

TEST_CASE("A 3MF without a plate number leaves the firmware's default plate", "[SnapmakerPrinterAgent]")
{
    // plate_index counts from 1; 0 means no plate was chosen
    const PrintParams params = make_params(false, false, false, "", 0);

    CHECK_FALSE(SnapmakerPrinterAgent::build_start_local_print_params(params, "Cube.gcode.3mf").contains("print_plate"));
}
