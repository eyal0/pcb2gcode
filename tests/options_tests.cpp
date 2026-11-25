#define BOOST_TEST_MODULE options tests
#include <boost/test/unit_test.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include "options.hpp"
#include "option_metadata.hpp"
#include "units.hpp"
#include "available_drills.hpp"

#include <nlohmann/json.hpp>

#include <regex>
#include <set>
#include <sstream>
#include <utility>

using namespace std;
using json = nlohmann::json;

BOOST_AUTO_TEST_SUITE(options_tests)

void parse(const std::string& args) {
  std::vector<std::string> words;
  boost::split(words, args, boost::is_any_of(" "), boost::token_compress_on);
  std::vector<const char*> argc(words.size());
  int argv = words.size();
  for (unsigned int i = 0; i < words.size(); i++) {
    argc[i] = words[i].c_str();
  }
  cerr << endl << "Parsing: " << args << endl;
  options::get_vm().clear();
  options::parse(argv, &argc[0]);
}

ErrorCodes get_error_code(const std::string& args) {
  try {
    parse(args);
  } catch (const pcb2gcode_parse_exception& e) {
    cerr << e.what() << endl;
    return e.code();
  }
  return ERR_OK;
}

template<typename variable_t>
variable_t get_value(const std::string& args, const std::string& variable) {
  BOOST_CHECK_EQUAL(get_error_code(args), ERR_OK);
  BOOST_CHECK_NO_THROW(options::get_vm().at(variable).as<variable_t>());
  return options::get_vm().at(variable).as<variable_t>();
}

int get_count(const std::string& args, const std::string& variable) {
  BOOST_CHECK_EQUAL(get_error_code(args), ERR_OK);
  return options::get_vm().count(variable);
}

// The code and message of the error from parsing and then checking args, or
// ERR_OK.
std::pair<ErrorCodes, std::string> get_check_error(const std::string& args) {
  try {
    parse(args);
    options::check_parameters();
  } catch (const pcb2gcode_parse_exception& e) {
    cerr << e.what() << endl;
    return {e.code(), e.what()};
  }
  return {ERR_OK, ""};
}

BOOST_AUTO_TEST_CASE(foo) {
  BOOST_CHECK_EQUAL(get_error_code("pcb2gcode --foo"), 101);
}

BOOST_AUTO_TEST_CASE(available_drills) {
  std::stringstream drills_text;
  auto drills = get_value<std::vector<AvailableDrills>>(
      "pcb2gcode --drills-available 5mm --drills-available 15mm",
      "drills-available");
  drills_text << drills;
  BOOST_CHECK_EQUAL(drills_text.str(), "0.005 m, 0.015 m");
}

BOOST_AUTO_TEST_CASE(offset) {
  BOOST_CHECK_EQUAL(get_count("pcb2gcode --offset 5mm", "offset"), 1);
  BOOST_CHECK_EQUAL(get_value<Length>("pcb2gcode --offset 5mm", "offset"), Length(0));
  BOOST_CHECK_EQUAL(get_value<std::vector<CommaSeparated<Length>>>("pcb2gcode --offset 5mm", "mill-diameters"),
                    std::vector<CommaSeparated<Length>>{{parse_unit<Length>("10mm")}});
}

BOOST_AUTO_TEST_CASE(mill_diameters) {
  BOOST_CHECK_EQUAL(get_value<std::vector<CommaSeparated<Length>>>("pcb2gcode --mill-diameters 1mm,2mm 3mm --mill-diameters 4mm", "mill-diameters"),
                    (std::vector<CommaSeparated<Length>>{
                      {parse_unit<Length>("1mm"), parse_unit<Length>("2mm")},
                      {parse_unit<Length>("3mm")},
                      {parse_unit<Length>("4mm")}
                    }));
}

BOOST_AUTO_TEST_CASE(milling_overlap) {
  BOOST_CHECK_EQUAL(
      (get_value<boost::variant<Length, Percent>>("pcb2gcode", "milling-overlap")),
      (boost::variant<Length, Percent>(parse_unit<Percent>("50%"))));
  BOOST_CHECK_EQUAL(
      (get_value<boost::variant<Length, Percent>>("pcb2gcode --milling-overlap 10%", "milling-overlap")),
      (boost::variant<Length, Percent>(parse_unit<Percent>("10%"))));
  BOOST_CHECK_EQUAL(
      (get_value<boost::variant<Length, Percent>>("pcb2gcode --milling-overlap 1mm", "milling-overlap")),
      (boost::variant<Length, Percent>(parse_unit<Length>("1mm"))));
  BOOST_CHECK_THROW(
      (parse("pcb2gcode --milling-overlap 1rpm")),
      pcb2gcode_parse_exception);
}

BOOST_AUTO_TEST_CASE(bridges_need_a_width) {
  BOOST_CHECK_EQUAL(get_value<unsigned int>("pcb2gcode --noconfigfile", "bridgesnum"), 0U);
  BOOST_CHECK_EQUAL(get_value<unsigned int>("pcb2gcode --noconfigfile --bridges=1mm",
                                            "bridgesnum"), 2U);
  BOOST_CHECK_EQUAL(get_value<unsigned int>("pcb2gcode --noconfigfile --bridges=1mm "
                                            "--bridgesnum=0", "bridgesnum"), 0U);
}

BOOST_AUTO_TEST_CASE(mcp_option) {
  BOOST_CHECK_EQUAL(get_count("pcb2gcode --mcp", "mcp"), 1);
}

BOOST_AUTO_TEST_CASE(unknown_options_reported_together) {
  auto error = get_check_error("pcb2gcode --safety=3 --drill-diameters=0.8");
  BOOST_CHECK_EQUAL(error.first, ERR_UNKNOWNPARAMETER);
  BOOST_CHECK_NE(error.second.find("--safety=3"), std::string::npos);
  BOOST_CHECK_NE(error.second.find("--drill-diameters=0.8"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(stray_arguments_reported) {
  auto error = get_check_error("pcb2gcode board.gbr --zsafe 5mm extra");
  BOOST_CHECK_EQUAL(error.first, ERR_UNKNOWNPARAMETER);
  BOOST_CHECK_NE(error.second.find("Unexpected argument board.gbr"), std::string::npos);
  BOOST_CHECK_NE(error.second.find("Unexpected argument extra"), std::string::npos);
  // 5mm is the value of --zsafe.
  BOOST_CHECK_EQUAL(error.second.find("5mm"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_options_reported_together) {
  auto error = get_check_error("pcb2gcode --noconfigfile --back=board.gbr --drill=board.drl "
                               "--mill-feed=100 --mill-speed=15000 --drill-feed=50 "
                               "--drill-speed=10000");
  // The first problem decides the exit code.
  BOOST_CHECK_EQUAL(error.first, ERR_NOZSAFE);
  for (const std::string option : {"zsafe", "zchange", "zwork", "zdrill"}) {
    const std::string missing = "Missing --" + option + ",";
    BOOST_CHECK_NE(error.second.find(missing), std::string::npos);
    BOOST_CHECK_EQUAL(error.second.find(missing), error.second.rfind(missing));
  }
}

BOOST_AUTO_TEST_CASE(examples_in_errors_fix_them) {
  const std::string args = "pcb2gcode --noconfigfile --back=board.gbr --outline=outline.gbr "
                           "--drill=board.drl --al-back";
  auto error = get_check_error(args);
  BOOST_REQUIRE_NE(error.first, ERR_OK);
  std::string fixed = args;
  const std::regex example("For example: (--\\S+)");
  for (std::sregex_iterator it(error.second.begin(), error.second.end(), example), end;
       it != end; ++it) {
    fixed += " " + (*it)[1].str();
  }
  BOOST_CHECK_EQUAL(get_check_error(fixed).first, ERR_OK);
}

BOOST_AUTO_TEST_CASE(ignore_warnings_with_missing_options) {
  // A run with files to mill and drill can't do without these values.
  auto error = get_check_error("pcb2gcode --noconfigfile --ignore-warnings "
                               "--back=board.gbr --outline=outline.gbr --drill=board.drl");
  BOOST_CHECK_EQUAL(error.first, ERR_NOZSAFE);
  for (const std::string option : {"zsafe", "zchange", "zwork", "mill-feed", "mill-speed", "zcut",
                                   "cutter-diameter", "cut-feed", "cut-speed", "cut-infeed",
                                   "zdrill", "drill-feed", "drill-speed"}) {
    BOOST_CHECK_NE(error.second.find("Missing --" + option + ","), std::string::npos);
  }
  // Without files, nothing needs them.
  BOOST_CHECK_EQUAL(get_check_error("pcb2gcode --noconfigfile --ignore-warnings").first, ERR_OK);
  // The other problems are still ignored.
  const std::string milling = "pcb2gcode --noconfigfile --ignore-warnings --back=board.gbr "
                              "--zsafe=-1mm --zchange=10mm --zwork=-0.1mm --mill-feed=100mm/min "
                              "--mill-speed=10000rpm --tile-x=0";
  BOOST_CHECK_EQUAL(get_check_error(milling).first, ERR_OK);
  // The autoleveller's values are needed only for a layer that it levels.
  BOOST_CHECK_EQUAL(get_check_error(milling + " --al-front").first, ERR_OK);
  BOOST_CHECK_EQUAL(get_check_error(milling + " --al-back").first, ERR_NOSOFTWARE);
}

BOOST_AUTO_TEST_CASE(ignore_warnings_with_unreadable_config_values) {
  const std::string config = " --config=tests/data/broken_examples/unreadable_value/millproject";
  // A run stops at the first value that it can't read.
  BOOST_CHECK_EQUAL(get_check_error("pcb2gcode" + config).first, ERR_INVALIDPARAMETER);
  // --ignore-warnings goes on without them.  The options keep their defaults,
  // a repeated option keeps its first value, the rest of the file is read, and
  // the run stops only for a value that it needs.
  const auto error = get_check_error("pcb2gcode --ignore-warnings" + config);
  BOOST_CHECK_EQUAL(error.first, ERR_NOMILLFEED);
  BOOST_CHECK_EQUAL(error.second, "Error: Missing --mill-feed, the milling feed rate. For "
                                  "example: --mill-feed=600mm/min");
  const po::variables_map& vm = options::get_vm();
  BOOST_CHECK(vm["mill-feed-direction"].as<MillFeedDirection::MillFeedDirection>() ==
              MillFeedDirection::ANY);
  BOOST_CHECK_EQUAL(vm.count("mill-feed"), 0U);
  BOOST_CHECK_EQUAL(vm["mill-speed"].as<Rpm>().asRpm(1), 10000);
  // A value on the command line still wins.
  BOOST_CHECK_EQUAL(get_check_error("pcb2gcode --ignore-warnings --mill-feed=100mm/min" +
                                    config).first,
                    ERR_OK);
}

BOOST_AUTO_TEST_CASE(zero_tolerance) {
  const std::string start = "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm";
  auto error = get_check_error(start + " --tolerance=0");
  BOOST_CHECK_EQUAL(error.first, ERR_NEGATIVETOLERANCE);
  BOOST_CHECK_NE(error.second.find("--tolerance must be greater than 0"), std::string::npos);
  BOOST_CHECK_EQUAL(get_check_error(start + " --tolerance=-1").first, ERR_NEGATIVETOLERANCE);
  BOOST_CHECK_EQUAL(get_check_error(start + " --g64=0").first, ERR_NEGATIVETOLERANCE);
  BOOST_CHECK_EQUAL(get_check_error(start + " --tolerance=0.0001").first, ERR_OK);
}

BOOST_AUTO_TEST_CASE(zchange_below_zdrill) {
  auto error = get_check_error("pcb2gcode --noconfigfile --drill=board.drl --zsafe=5mm "
                               "--zchange=-3mm --zdrill=-2.5mm --drill-feed=500mm/min "
                               "--drill-speed=10000rpm");
  BOOST_CHECK_EQUAL(error.first, ERR_ZSAFELOWERZCHANGE);
  BOOST_CHECK_NE(error.second.find("--zchange-absolute"), std::string::npos);
  BOOST_CHECK_EQUAL(get_check_error("pcb2gcode --noconfigfile --drill=board.drl --zsafe=5mm "
                                    "--zchange=-3mm --zdrill=-2.5mm --drill-feed=500mm/min "
                                    "--drill-speed=10000rpm --zchange-absolute").first,
                    ERR_OK);
}

BOOST_AUTO_TEST_CASE(range_rules) {
  const string start = "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm";
  BOOST_CHECK_EQUAL(get_check_error(start + " --tile-x=0").first, ERR_NEGATIVETILEX);
  BOOST_CHECK_EQUAL(get_check_error(start + " --tile-x=1").first, ERR_OK);
  BOOST_CHECK_EQUAL(get_check_error(start + " --al-front --software=linuxcnc --al-x=0 "
                                    "--al-y=1mm --al-probefeed=100mm/min").first,
                    ERR_NEGATIVEALX);

  const string outline = start + " --outline=outline.gbr --zcut=-2mm --cutter-diameter=2mm "
                         "--cut-feed=100mm/min --cut-speed=10000rpm";
  BOOST_CHECK_EQUAL(get_check_error(outline + " --cut-infeed=0.0009in").first, ERR_LOWCUTINFEED);
  BOOST_CHECK_EQUAL(get_check_error(outline + " --cut-infeed=0.001in").first, ERR_OK);
  // Bare lengths are inches, or millimeters with --metric.
  BOOST_CHECK_EQUAL(get_check_error(outline + " --cut-infeed=0.0009").first, ERR_LOWCUTINFEED);
  BOOST_CHECK_EQUAL(get_check_error(outline + " --metric --cut-infeed=0.03").first, ERR_OK);

  const string milldrill = start + " --drill=board.drl --zdrill=-2mm --drill-feed=100mm/min "
                           "--drill-speed=10000rpm --min-milldrill-hole-diameter=0 --zcut=-2mm "
                           "--cutter-diameter=1mm --cut-feed=100mm/min --cut-speed=10000rpm "
                           "--cut-infeed=1mm";
  BOOST_CHECK_EQUAL(get_check_error(milldrill).first, ERR_OK);
  BOOST_CHECK_EQUAL(get_check_error(milldrill + " --milldrill-stepover=100%").first, ERR_OK);
  BOOST_CHECK_EQUAL(get_check_error(milldrill + " --milldrill-stepover=150%").first,
                    ERR_INVALIDMILLDRILLSTEPOVER);
  BOOST_CHECK_EQUAL(get_check_error(milldrill + " --milldrill-stepover=0").first,
                    ERR_INVALIDMILLDRILLSTEPOVER);
  BOOST_CHECK_EQUAL(get_check_error(milldrill + " --min-milldrill-entry-diameter=100%").first,
                    ERR_LOWMILLDRILLENTRY);
  BOOST_CHECK_EQUAL(get_check_error(milldrill + " --max-milldrill-entry-diameter=140%").first,
                    ERR_MILLDRILLENTRYORDER);
}

BOOST_AUTO_TEST_CASE(deprecated_sides) {
  const string drill = "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm --drill=board.drl "
                       "--zdrill=-2mm --drill-feed=100mm/min --drill-speed=10000rpm";
  BOOST_CHECK_EQUAL(get_check_error(drill + " --drill-front").first, ERR_OK);
  BOOST_CHECK_EQUAL(get_check_error(drill + " --drill-front --drill-side=back").first,
                    ERR_BOTHDRILLFRONTSIDE);
  const string outline = "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm "
                         "--outline=outline.gbr --zcut=-2mm --cutter-diameter=2mm "
                         "--cut-feed=100mm/min --cut-speed=10000rpm --cut-infeed=1mm";
  BOOST_CHECK_EQUAL(get_check_error(outline + " --cut-front=false").first, ERR_OK);
  BOOST_CHECK_EQUAL(get_check_error(outline + " --cut-front --cut-side=front").first,
                    ERR_BOTHCUTFRONTSIDE);
}

// What check_parameters() prints to stderr for args.
string check_warnings(const string& args) {
  std::stringstream warnings;
  struct RestoreCerr {
    std::streambuf* buffer;
    ~RestoreCerr() { cerr.rdbuf(buffer); }
  } restore{cerr.rdbuf(warnings.rdbuf())};
  BOOST_CHECK_EQUAL(get_check_error(args).first, ERR_OK);
  return warnings.str();
}

BOOST_AUTO_TEST_CASE(warnings) {
  const string start = "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm";
  BOOST_CHECK_NE(check_warnings(start + " --tolerance=1").find("high tolerance"), string::npos);
  BOOST_CHECK_EQUAL(check_warnings(start + " --tolerance=0.001").find("Warning"), string::npos);
  BOOST_CHECK_NE(check_warnings(start + " --svg=board.svg").find("--svg is deprecated"),
                 string::npos);
  BOOST_CHECK_NE(check_warnings(start + " --back=board.gbr --zwork=0.1mm --mill-feed=1 "
                                "--mill-speed=1").find("--zwork) is greater than zero"),
                 string::npos);
  BOOST_CHECK_NE(check_warnings(start + " --drill=board.drl --zdrill=-2mm --drill-feed=1 "
                                "--drill-speed=1 --drill-front").find("drill-front is deprecated"),
                 string::npos);
  BOOST_CHECK_NE(check_warnings(start + " --outline=outline.gbr --zcut=-2mm "
                                "--cutter-diameter=2mm --cut-feed=1 --cut-speed=1 "
                                "--cut-infeed=1mm --cut-front").find("cut-front is deprecated"),
                 string::npos);
}

BOOST_AUTO_TEST_CASE(conditions) {
  parse("pcb2gcode --noconfigfile --drill=board.drl");
  const po::variables_map& vm = options::get_vm();
  const auto set = &OptionCondition::set;
  // An option without a value is never equal to anything.
  BOOST_CHECK(!OptionCondition::equal("software", "linuxcnc").holds(vm));
  BOOST_CHECK(OptionCondition::equal("drill-side", "auto").holds(vm));
  BOOST_CHECK(!OptionCondition::given("drill-side").holds(vm));
  BOOST_CHECK(OptionCondition::given("drill").holds(vm));
  // Only numbers can be compared with inf.
  BOOST_CHECK_THROW(OptionCondition::finite("drill").holds(vm), std::logic_error);
  BOOST_CHECK_THROW(OptionCondition::finite("mill-diameters").holds(vm), std::logic_error);
  BOOST_CHECK_THROW(OptionCondition::finite("pre-milling-gcode").holds(vm), std::logic_error);
  BOOST_CHECK(!(set("front") || set("back") || set("outline")).holds(vm));
  BOOST_CHECK((set("drill") && !set("front") && set("zchange-absolute")).holds(vm));
  // Chains stay flat.
  BOOST_CHECK_EQUAL((set("front") || set("back") || set("outline")).to_json().dump(),
                    R"({"any":[{"set":"front"},{"set":"back"},{"set":"outline"}]})");
  BOOST_CHECK_EQUAL((set("a") && (set("b") && set("c"))).to_json().dump(),
                    R"({"all":[{"set":"a"},{"set":"b"},{"set":"c"}]})");
}

// A catalog made by hand, for what pcb2gcode's own options don't use.
BOOST_AUTO_TEST_CASE(small_catalog) {
  po::options_description description;
  description.add_options()
      ("width", po::value<Length>(), "a width")
      ("depth", po::value<Length>()->default_value(parse_unit<Length>("1mm")), "a depth");
  const std::vector<OptionGroup> groups{{"test", "Test options", "always", false, {"width"}}};
  const std::vector<NamedCondition> conditions{{"always", "Always.", OptionCondition()}};
  const std::map<string, OptionDetails> details;
  const std::vector<OptionRule> rules{
      OptionRule::range("width").below("2mm").fails_with(ERR_INVALIDPARAMETER, "too wide"),
      OptionRule::greater_or_equal("width", "depth").fails_with(ERR_INVALIDPARAMETER, "shallow")};
  const OptionCatalog catalog{description, groups, conditions, details, rules};

  for (const auto& width_and_errors : std::vector<std::pair<string, std::vector<string>>>{
           {"1mm", {}}, {"2mm", {"too wide"}}, {"0.5mm", {"shallow"}}}) {
    po::variables_map vm;
    po::store(po::command_line_parser(std::vector<string>{"--width=" + width_and_errors.first})
                  .options(description).run(), vm);
    std::vector<string> errors;
    for (const auto& rule : rules) {
      const auto error = rule.check(vm, catalog, 1);
      if (error) {
        errors.push_back(error->what);
      }
    }
    BOOST_CHECK(errors == width_and_errors.second);
  }
  const json dumped = json::parse(dump_catalog(catalog));
  BOOST_CHECK_EQUAL(dumped["rules"][0]["max"], "2mm");
  BOOST_CHECK_EQUAL(dumped["rules"][0]["max_exclusive"], true);
  BOOST_CHECK_EQUAL(dumped["options"][0]["range"], (json{{"max", "2mm"}, {"max_exclusive", true}}));

  // Mistakes in the catalog are bugs, so they throw.
  po::variables_map vm;
  vm.insert({"height", po::variable_value(boost::any(Length(1)), false)});
  BOOST_CHECK_THROW(OptionRule::range("height").at_least("0").check(vm, catalog, 1),
                    std::logic_error);
  BOOST_CHECK_THROW(OptionRule::required("height", "a height", "1mm").when("never")
                        .check(vm, catalog, 1),
                    std::logic_error);
  const std::vector<OptionGroup> wrong_groups{{"test", "Test options", "always", false, {"height"}}};
  BOOST_CHECK_THROW(dump_catalog(OptionCatalog{description, wrong_groups, conditions, details,
                                               rules}),
                    std::logic_error);
}

BOOST_AUTO_TEST_CASE(rule_errors_name_their_options) {
  parse("pcb2gcode --noconfigfile --zsafe=1mm --zchange=10mm --back=board.gbr --zwork=2mm "
        "--mill-feed=1 --mill-speed=1");
  std::vector<OptionError> errors;
  check_rules(options::catalog(), options::get_vm(), errors);
  BOOST_REQUIRE_EQUAL(errors.size(), 1U);
  BOOST_CHECK_EQUAL(errors[0].code, ERR_ZSAFELOWERZWORK);
  BOOST_CHECK(errors[0].options == (std::vector<std::string>{"zsafe", "zwork"}));
  BOOST_CHECK_EQUAL(errors[0].rule, "greater:zsafe,zwork");
}

// The tolerance in inches, as pcb2gcode uses it.
double tolerance(const string& args) {
  BOOST_CHECK_EQUAL(get_error_code(boost::trim_copy("pcb2gcode --noconfigfile " + args)), ERR_OK);
  const po::variables_map& vm = options::get_vm();
  const double unit = vm["metric"].as<bool>() ? 1. / 25.4 : 1;
  return vm["tolerance"].as<Length>().asInch(unit);
}

BOOST_AUTO_TEST_CASE(tolerance_without_unit) {
  // Exactly the numbers that pcb2gcode used when --tolerance was a plain number.
  BOOST_CHECK_EQUAL(tolerance(""), 0.0004);
  BOOST_CHECK_EQUAL(tolerance("--metric"), 0.0004);
  BOOST_CHECK_EQUAL(tolerance("--tolerance=0.001"), 0.001);
  BOOST_CHECK_EQUAL(tolerance("--tolerance 0.001"), 0.001);
  BOOST_CHECK_EQUAL(tolerance("--metric --tolerance=0.006"), 0.006 * (1. / 25.4));
  BOOST_CHECK_EQUAL(tolerance("--tolerance=4e-4"), 0.0004);
  BOOST_CHECK_EQUAL(tolerance("--metric --tolerance=1E-2"), 0.01 * (1. / 25.4));
  BOOST_CHECK_EQUAL(tolerance("--g64=0.001"), 0.001);
  BOOST_CHECK_EQUAL(tolerance("--metric --g64=0.02"), 0.02 * (1. / 25.4));
}

BOOST_AUTO_TEST_CASE(tolerance_with_unit) {
  BOOST_CHECK_CLOSE(tolerance("--tolerance=0.01mm"), 0.01 / 25.4, 1e-9);
  // The unit wins over --metric.
  BOOST_CHECK_CLOSE(tolerance("--metric --tolerance=0.001in"), 0.001, 1e-9);
  BOOST_CHECK_CLOSE(tolerance("--metric --tolerance=2thou"), 0.002, 1e-9);
  BOOST_CHECK_CLOSE(tolerance("--g64=0.01mm"), 0.01 / 25.4, 1e-9);
  BOOST_CHECK_EQUAL(get_error_code("pcb2gcode --noconfigfile --tolerance=0.01mm/s"),
                    ERR_UNKNOWNPARAMETER);
  BOOST_CHECK_EQUAL(get_error_code("pcb2gcode --noconfigfile --tolerance=0.01mm --g64=0.01mm"),
                    ERR_BOTHTOLERANCEG64);
  BOOST_CHECK_EQUAL(get_check_error("pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm "
                                    "--tolerance=-0.01mm").first,
                    ERR_NEGATIVETOLERANCE);
  BOOST_CHECK_EQUAL(get_check_error("pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm "
                                    "--tolerance=0mm").first,
                    ERR_NEGATIVETOLERANCE);
  BOOST_CHECK_EQUAL(get_check_error("pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm "
                                    "--tolerance=0.001mm").first,
                    ERR_OK);
}

BOOST_AUTO_TEST_CASE(mirror_yaxis_flag) {
  BOOST_CHECK_EQUAL(get_value<bool>("pcb2gcode --noconfigfile", "mirror-yaxis"), false);
  BOOST_CHECK_EQUAL(get_value<bool>("pcb2gcode --noconfigfile --mirror-yaxis", "mirror-yaxis"),
                    true);
  BOOST_CHECK_EQUAL(get_value<bool>("pcb2gcode --mirror-yaxis --noconfigfile", "mirror-yaxis"),
                    true);
  BOOST_CHECK_EQUAL(get_value<bool>("pcb2gcode --noconfigfile --mirror-yaxis=false",
                                    "mirror-yaxis"),
                    false);
  // The value can also be the next argument.
  BOOST_CHECK_EQUAL(get_value<bool>("pcb2gcode --noconfigfile --mirror-yaxis false",
                                    "mirror-yaxis"),
                    false);
  BOOST_CHECK_EQUAL(get_value<bool>("pcb2gcode --noconfigfile --mirror-yaxis true --zsafe=1mm",
                                    "mirror-yaxis"),
                    true);
  BOOST_CHECK_EQUAL(get_error_code("pcb2gcode --noconfigfile --mirror-yaxis board.gbr"),
                    ERR_UNKNOWNPARAMETER);
}

json dump() {
  return json::parse(options::dump_options());
}

json find_option(const json& dumped, const string& name) {
  for (const auto& option : dumped["options"]) {
    if (option["name"] == name) {
      return option;
    }
  }
  BOOST_FAIL("No option " + name);
  return json();
}

// Reads the texts with the option's syntax and writes the value the way
// pcb2gcode writes values.
std::vector<string> reformat(const string& option, const std::vector<string>& texts) {
  const po::option_description& description = options::catalog().options.find(option, false);
  boost::any value;
  for (const auto& text : texts) {
    description.semantic()->parse(value, {text}, false);
  }
  const boost::optional<std::vector<string>> formatted = format_option_value(value);
  BOOST_REQUIRE(formatted);
  return *formatted;
}

// The text, or every text of a repeatable option, reads back unchanged.
void check_reads_back(const string& option, const json& text) {
  BOOST_TEST_CONTEXT("--" << option << "=" << text) {
    const std::vector<string> texts = text.is_array() ? text.get<std::vector<string>>()
                                                      : std::vector<string>{text.get<string>()};
    if (!texts.empty()) {
      BOOST_CHECK(reformat(option, texts) == texts);
    }
  }
}

// The values that the condition compares with read back unchanged.
void check_condition_literals(const json& condition) {
  if (condition.is_boolean()) {
    return;
  }
  for (const auto& operand : condition.items()) {
    if (operand.key() == "equal") {
      check_reads_back(operand.value()[0].get<string>(), operand.value()[1]);
    } else if (operand.key() == "not") {
      check_condition_literals(operand.value());
    } else if (operand.key() == "any" || operand.key() == "all") {
      for (const auto& sub_condition : operand.value()) {
        check_condition_literals(sub_condition);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(dump_lists_every_option) {
  const json dumped = dump();
  BOOST_CHECK_EQUAL(dumped["format_version"], 1);
  BOOST_CHECK_EQUAL(dumped["program"], "pcb2gcode");
  BOOST_CHECK(dumped["version"].is_string());
  std::set<string> group_ids;
  for (const auto& group : dumped["groups"]) {
    BOOST_CHECK(group_ids.insert(group["id"].get<string>()).second);
    BOOST_CHECK(!group["title"].get<string>().empty());
  }
  std::set<string> names;
  for (const auto& option : dumped["options"]) {
    BOOST_TEST_CONTEXT("--" << option["name"]) {
      BOOST_CHECK(names.insert(option["name"].get<string>()).second);
      for (const char* key : {"group", "description", "type", "default", "used_when"}) {
        BOOST_CHECK(option.contains(key));
      }
      BOOST_CHECK(group_ids.count(option["group"].get<string>()) == 1);
      BOOST_CHECK(dumped["types"].contains(option["type"].get<string>()));
      BOOST_CHECK(dumped["conditions"].contains(option["used_when"].get<string>()));
      if (option.contains("required_when")) {
        BOOST_CHECK(dumped["conditions"].contains(option["required_when"].get<string>()));
      }
    }
  }
  BOOST_CHECK_EQUAL(names.size(), options::catalog().options.options().size());
  std::set<int> codes;
  for (const auto& code : dumped["exit_codes"]) {
    codes.insert(code["code"].get<int>());
  }
  for (const auto& rule : dumped["rules"]) {
    BOOST_TEST_CONTEXT(rule["message"]) {
      BOOST_CHECK(dumped["conditions"].contains(rule["when"].get<string>()));
      BOOST_CHECK(codes.count(rule["code"].get<int>()) == 1);
      for (const auto& option : rule["options"]) {
        BOOST_CHECK(names.count(option.get<string>()) == 1);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(dump_values_read_back) {
  const json dumped = dump();
  for (const auto& option : dumped["options"]) {
    const string name = option["name"].get<string>();
    // pcb2gcode can't read inf, so the only way to get that default is to
    // leave the option out.
    if (!option["default"].is_null() && option["default"] != "inf") {
      check_reads_back(name, option["default"]);
    }
    if (option.contains("implicit")) {
      check_reads_back(name, option["implicit"]);
    }
  }
  for (const auto& rule : dumped["rules"]) {
    for (const char* key : {"min", "max", "example"}) {
      if (rule.contains(key)) {
        check_reads_back(rule["options"][0].get<string>(), rule[key]);
      }
    }
    if (rule.contains("if")) {
      check_condition_literals(rule["if"]);
    }
    if (rule.contains("unless")) {
      check_condition_literals(rule["unless"]);
    }
  }
  for (const auto& condition : dumped["conditions"]) {
    check_condition_literals(condition["expression"]);
  }
}

BOOST_AUTO_TEST_CASE(dump_types_read_back) {
  const json types = dump()["types"];
  for (const auto& text : types["bool"]["true"]) {
    BOOST_CHECK(reformat("metric", {boost::to_upper_copy(text.get<string>())}) ==
                std::vector<string>{"true"});
  }
  for (const auto& text : types["bool"]["false"]) {
    BOOST_CHECK(reformat("metric", {text.get<string>()}) == std::vector<string>{"false"});
  }
  // Every unit name reads back as the main name.
  for (const auto& type_and_option : std::vector<std::pair<string, string>>{
           {"length", "zsafe"}, {"time", "spinup-time"}, {"rpm", "mill-speed"},
           {"percent", "milldrill-stepover"}}) {
    for (const auto& unit : types[type_and_option.first]["units"]) {
      const string name = unit["name"].get<string>();
      std::vector<string> names = unit["aliases"].get<std::vector<string>>();
      names.push_back(name);
      for (const string& alias : names) {
        BOOST_TEST_CONTEXT("--" << type_and_option.second << "=2 " << alias) {
          BOOST_CHECK(reformat(type_and_option.second, {"2 " + alias}) ==
                      std::vector<string>{"2" + name});
        }
      }
    }
  }
  BOOST_CHECK(reformat("mill-feed", {"2 inches per minute"}) == std::vector<string>{"2in/min"});
  BOOST_CHECK(reformat("mill-speed", {"2 cycles/s"}) == std::vector<string>{"2rev/s"});
  BOOST_CHECK_EQUAL(types["length"]["units"][0]["name"], "mm");
  BOOST_CHECK_EQUAL(types["length"]["units"][0]["factor"], 0.001);
}

BOOST_AUTO_TEST_CASE(dump_option_details) {
  const json dumped = dump();

  const json zwork = find_option(dumped, "zwork");
  BOOST_CHECK_EQUAL(zwork["group"], "milling");
  BOOST_CHECK_EQUAL(zwork["type"], "length");
  BOOST_CHECK(zwork["default"].is_null());
  BOOST_CHECK_EQUAL(zwork["example"], "-0.05mm");
  BOOST_CHECK_EQUAL(zwork["required_when"], "milling");
  BOOST_CHECK_EQUAL(zwork["used_when"], "milling");

  const json optimise = find_option(dumped, "optimise");
  BOOST_CHECK_EQUAL(optimise["default"], "0.0001in");
  BOOST_CHECK_EQUAL(optimise["implicit"], "0.0001in");
  BOOST_CHECK_EQUAL(find_option(dumped, "g0-vertical-speed")["default"], "50in/min");
  BOOST_CHECK_EQUAL(find_option(dumped, "spinup-time")["default"], "1ms");
  BOOST_CHECK_EQUAL(find_option(dumped, "min-milldrill-hole-diameter")["default"], "inf");
  BOOST_CHECK_EQUAL(find_option(dumped, "milling-overlap")["type"], "length_or_percent");
  BOOST_CHECK_EQUAL(find_option(dumped, "milling-overlap")["default"], "50%");

  const json direction = find_option(dumped, "mill-feed-direction");
  BOOST_CHECK_EQUAL(direction["type"], "enum");
  BOOST_CHECK_EQUAL(direction["default"], "any");
  BOOST_CHECK_EQUAL(direction["choices"][1],
                    (json{{"value", "climb"}, {"aliases", json::array({"clockwise"})}}));
  BOOST_CHECK_EQUAL(find_option(dumped, "software")["choices"].size(), 4U);

  const json drills = find_option(dumped, "drills-available");
  BOOST_CHECK_EQUAL(drills["type"], "available_drill");
  BOOST_CHECK_EQUAL(drills["default"], json::array());
  BOOST_CHECK_EQUAL(drills["repeatable"], true);
  BOOST_CHECK_EQUAL(drills["comma_separated"], true);
  BOOST_CHECK_EQUAL(drills["multitoken"], true);
  BOOST_CHECK_EQUAL(find_option(dumped, "mill-diameters")["default"], json::array({"0"}));
  const json gcode = find_option(dumped, "pre-milling-gcode");
  BOOST_CHECK_EQUAL(gcode["repeatable"], true);
  BOOST_CHECK(!gcode.contains("comma_separated"));
  BOOST_CHECK(!gcode.contains("multitoken"));

  const json tolerance = find_option(dumped, "tolerance");
  BOOST_CHECK_EQUAL(tolerance["type"], "length");
  // It depends on --g64.
  BOOST_CHECK(tolerance["default"].is_null());
  BOOST_CHECK_EQUAL(tolerance["default_note"], "0.0004in, or --g64 if it's given");
  BOOST_CHECK_EQUAL(tolerance["range"], (json{{"min", "0"}, {"min_exclusive", true}}));
  BOOST_CHECK_EQUAL(find_option(dumped, "g64")["type"], "length");

  const json help = find_option(dumped, "help");
  BOOST_CHECK_EQUAL(help["type"], "none");
  BOOST_CHECK_EQUAL(help["short"], "?");
  BOOST_CHECK_EQUAL(help["cli_only"], true);
  BOOST_CHECK_EQUAL(find_option(dumped, "version")["short"], "V");
  BOOST_CHECK_EQUAL(find_option(dumped, "dump-options")["cli_only"], true);
  BOOST_CHECK(!find_option(dumped, "zsafe").contains("cli_only"));

  const json drill_front = find_option(dumped, "drill-front");
  BOOST_CHECK_EQUAL(drill_front["deprecated"], true);
  BOOST_CHECK_EQUAL(drill_front["replaced_by"], "drill-side");
  BOOST_CHECK_EQUAL(drill_front["value_map"], (json{{"false", "back"}, {"true", "front"}}));
  BOOST_CHECK_EQUAL(drill_front["implicit"], "true");
  BOOST_CHECK_EQUAL(find_option(dumped, "g64")["replaced_by"], "tolerance");
  BOOST_CHECK(!find_option(dumped, "g64").contains("value_map"));
  // A number of passes doesn't carry over to a length.
  BOOST_CHECK_EQUAL(find_option(dumped, "extra-passes")["deprecated"], true);
  BOOST_CHECK(!find_option(dumped, "extra-passes").contains("replaced_by"));
  BOOST_CHECK_EQUAL(find_option(dumped, "vectorial")["deprecated"], true);
  BOOST_CHECK_EQUAL(find_option(dumped, "mirror-yaxis")["implicit"], "true");

  const json front = find_option(dumped, "front");
  BOOST_CHECK_EQUAL(front["path"], "input_file");
  BOOST_CHECK_EQUAL(front["file_format"], "gerber");
  BOOST_CHECK_EQUAL(front["used_when"], "always");
  BOOST_CHECK_EQUAL(find_option(dumped, "front-output")["relative_to"], "output-dir");
  const json manifest = find_option(dumped, "manifest");
  BOOST_CHECK_EQUAL(manifest["group"], "generic");
  BOOST_CHECK_EQUAL(manifest["type"], "string");
  BOOST_CHECK(manifest["default"].is_null());
  BOOST_CHECK_EQUAL(manifest["used_when"], "always");
  BOOST_CHECK_EQUAL(manifest["path"], "output_file");
  BOOST_CHECK_EQUAL(manifest["file_format"], "json");
  BOOST_CHECK_EQUAL(manifest["relative_to"], "output-dir");
  BOOST_CHECK_EQUAL(find_option(dumped, "output-dir")["path"], "directory");

  BOOST_CHECK_EQUAL(find_option(dumped, "cut-infeed")["range"], (json{{"min", "0.001in"}}));
  BOOST_CHECK_EQUAL(find_option(dumped, "tile-x")["range"], (json{{"min", "1"}}));
  const json stepover = find_option(dumped, "milldrill-stepover");
  BOOST_CHECK_EQUAL(stepover["range"],
                    (json{{"min", "0"}, {"min_exclusive", true}, {"max", "100%"}}));
  BOOST_CHECK_EQUAL(stepover["used_when"], "milldrilling");
}

BOOST_AUTO_TEST_CASE(dump_conditions) {
  const json conditions = dump()["conditions"];
  BOOST_CHECK_EQUAL(conditions["always"]["expression"], true);
  const json front = {{"set", "front"}};
  const json back = {{"set", "back"}};
  BOOST_CHECK_EQUAL(conditions["milling"]["expression"],
                    (json{{"any", json::array({front, back})}}));
  const json outline = {{"set", "outline"}};
  const json drill = {{"set", "drill"}};
  const json finite = {{"finite", "min-milldrill-hole-diameter"}};
  const json milldrilling = {{"all", json::array({drill, finite})}};
  BOOST_CHECK_EQUAL(conditions["milldrilling"]["expression"], milldrilling);
  BOOST_CHECK_EQUAL(conditions["cutting"]["expression"],
                    (json{{"any", json::array({outline, milldrilling})}}));
}

BOOST_AUTO_TEST_CASE(format_values) {
  BOOST_CHECK(reformat("pre-milling-gcode", {"M3", "G4 P1"}) ==
              (std::vector<string>{"M3", "G4 P1"}));
  BOOST_CHECK(reformat("drills-available", {"1mm:-0.1mm:+0.2mm,2mm", "3mm"}) ==
              (std::vector<string>{"1mm:-0.1mm:+0.2mm,2mm", "3mm"}));
  // No option is a double anymore, but a double is still written without an exponent.
  BOOST_CHECK(format_option_value(boost::any(2.5e-5)) ==
              boost::make_optional(std::vector<string>{"0.000025"}));
  // pcb2gcode has no options of this type.
  BOOST_CHECK(!format_option_value(boost::any(1.5f)));
}

BOOST_AUTO_TEST_CASE(dump_rule_ids) {
  const json dumped = dump();
  std::set<string> ids;
  for (const auto& rule : dumped["rules"]) {
    BOOST_CHECK(ids.insert(rule["id"].get<string>()).second);
    BOOST_CHECK_EQUAL(rule["id"], rule["kind"].get<string>() + ":" +
                      boost::algorithm::join(rule["options"].get<std::vector<string>>(), ","));
  }
}

// The exit code and the output of pcb2gcode --check-options with the words of
// args.
std::pair<int, json> check_options(const std::string& args) {
  std::vector<std::string> words;
  boost::split(words, args, boost::is_any_of(" "), boost::token_compress_on);
  std::vector<const char*> argv;
  for (const auto& word : words) {
    argv.push_back(word.c_str());
  }
  cerr << endl << "Checking: " << args << endl;
  std::ostringstream out;
  const int code = options::check_options(static_cast<int>(argv.size()), argv.data(), out);
  const json report = json::parse(out.str());
  BOOST_CHECK_EQUAL(report["format_version"], 1);
  return {code, report};
}

json error_json(const std::vector<string>& option_names, const string& rule,
                const string& message, ErrorCodes code) {
  return {{"options", option_names}, {"rule", rule}, {"message", message}, {"code", code}};
}

BOOST_AUTO_TEST_CASE(check_requested) {
  const char* with[] = {"pcb2gcode", "--zsafe=1", "--check-options"};
  BOOST_CHECK(options::check_requested(3, with));
  const char* without[] = {"pcb2gcode", "--check-options=yes", "--zsafe=1"};
  BOOST_CHECK(!options::check_requested(3, without));
}

BOOST_AUTO_TEST_CASE(check_options_without_problems) {
  const auto checked =
      check_options("pcb2gcode --check-options --noconfigfile --zsafe=5mm --zchange=10mm");
  BOOST_CHECK_EQUAL(checked.first, ERR_OK);
  BOOST_CHECK_EQUAL(checked.second["errors"], json::array());
  BOOST_CHECK_EQUAL(checked.second["warnings"], json::array());
}

BOOST_AUTO_TEST_CASE(check_options_lists_every_error) {
  const string args = "pcb2gcode --noconfigfile --zsafe=1mm --zchange=10mm --back=board.gbr "
                      "--zwork=2mm --mill-speed=10000rpm";
  const auto checked = check_options(args + " --check-options");
  BOOST_CHECK_EQUAL(checked.first, ERR_NOMILLFEED);
  const string missing_feed = "Missing --mill-feed, the milling feed rate. For example: "
                              "--mill-feed=600mm/min";
  const string zsafe_low = "--zsafe must be higher than --zwork.";
  BOOST_CHECK_EQUAL(checked.second["errors"],
                    json::array({error_json({"mill-feed"}, "required:mill-feed", missing_feed,
                                            ERR_NOMILLFEED),
                                 error_json({"zsafe", "zwork"}, "greater:zsafe,zwork", zsafe_low,
                                            ERR_ZSAFELOWERZWORK)}));
  BOOST_CHECK_EQUAL(checked.second["warnings"],
                    json::array({{{"options", {"zwork"}},
                                  {"message",
                                   "Warning: Engraving depth (--zwork) is greater than zero!"}}}));
  // A run stops with the same errors.
  const auto run = get_check_error(args);
  BOOST_CHECK_EQUAL(run.first, ERR_NOMILLFEED);
  BOOST_CHECK_EQUAL(run.second, "Error: " + missing_feed + "\nError: " + zsafe_low);
}

// The errors that stop a run before it reads any files, which --check-options
// lists too.
BOOST_AUTO_TEST_CASE(check_options_matches_a_run) {
  for (const string args : {
           "pcb2gcode --noconfigfile --back=board.gbr --drill=board.drl --mill-feed=100 "
           "--mill-speed=15000 --drill-feed=50 --drill-speed=10000",
           "pcb2gcode --noconfigfile --back=board.gbr --outline=outline.gbr --drill=board.drl "
           "--al-back",
           "pcb2gcode --noconfigfile --drill=board.drl --zsafe=5mm --zchange=-3mm --zdrill=-2.5mm "
           "--drill-feed=500mm/min --drill-speed=10000rpm",
           "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm --tile-x=0 "
           "--mill-feed-direction=climb",
           "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm --drill=board.drl --zdrill=-2mm "
           "--drill-feed=100mm/min --drill-speed=10000rpm --min-milldrill-hole-diameter=0 "
           "--zcut=-2mm --cutter-diameter=1mm --cut-feed=100mm/min --cut-speed=10000rpm "
           "--cut-infeed=0.0009in --milldrill-stepover=150% --max-milldrill-entry-diameter=140%",
           "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm --outline=outline.gbr --zcut=1mm "
           "--cut-front --cut-side=back",
           "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm --tolerance=0"}) {
    const auto run = get_check_error(args);
    const auto checked = check_options(args + " --check-options");
    BOOST_CHECK_EQUAL(checked.first, run.first);
    string text;
    for (const auto& error : checked.second["errors"]) {
      text += "Error: " + error["message"].get<string>() + "\n";
    }
    BOOST_CHECK_EQUAL(text, run.second + "\n");
  }
}

BOOST_AUTO_TEST_CASE(check_options_reports_unreadable_options) {
  const auto checked = check_options(
      "pcb2gcode --check-options --noconfigfile --zsafe=abc --zchange=10mm --tile-x=1.5 --foo=1 "
      "stray --zchange=2mm --back=board.gbr --zwork=-1mm --mill-feed=1 --mill-speed=-1");
  BOOST_CHECK_EQUAL(checked.first, ERR_UNKNOWNPARAMETER);
  const json& errors = checked.second["errors"];
  BOOST_REQUIRE_EQUAL(errors.size(), 6U);
  BOOST_CHECK_EQUAL(errors[0], error_json({"foo"}, "unknown_option", "Unknown option --foo=1",
                                          ERR_UNKNOWNPARAMETER));
  BOOST_CHECK_EQUAL(errors[1],
                    error_json({}, "unexpected_argument",
                               "Unexpected argument stray. Arguments need an option name, like "
                               "--front=board.gbr", ERR_UNKNOWNPARAMETER));
  for (size_t i = 2; i < 5; i++) {
    BOOST_CHECK_EQUAL(errors[i]["rule"], "invalid_value");
    BOOST_CHECK_EQUAL(errors[i]["code"], ERR_UNKNOWNPARAMETER);
  }
  BOOST_CHECK_EQUAL(errors[2]["options"], json::array({"zsafe"}));
  BOOST_CHECK_NE(errors[2]["message"].get<string>().find("'--zsafe' is invalid"), string::npos);
  BOOST_CHECK_EQUAL(errors[3]["message"], "the argument ('1.5') for option '--tile-x' is invalid");
  BOOST_CHECK_EQUAL(errors[4]["message"], "option '--zchange' cannot be specified more than once");
  // The rules still check the other options, but not the ones that couldn't
  // be read: no "Missing --zsafe".
  BOOST_CHECK_EQUAL(errors[5]["rule"], "range:mill-speed");
  // A run stops at the first problem.
  BOOST_CHECK_EQUAL(get_error_code("pcb2gcode --noconfigfile --zsafe=abc"), ERR_UNKNOWNPARAMETER);
}

BOOST_AUTO_TEST_CASE(check_options_with_unparsable_command_line) {
  // Nothing after a command line that can't be parsed is checked.
  const auto checked = check_options("pcb2gcode --check-options --noconfigfile --zsafe");
  BOOST_CHECK_EQUAL(checked.first, ERR_UNKNOWNPARAMETER);
  BOOST_CHECK_EQUAL(checked.second["errors"],
                    json::array({error_json({"zsafe"}, "invalid_value",
                                            "the required argument for option '--zsafe' is missing",
                                            ERR_UNKNOWNPARAMETER)}));
  BOOST_CHECK_EQUAL(checked.second["warnings"], json::array());
}

BOOST_AUTO_TEST_CASE(check_options_reads_config_files) {
  const string file = "tests/data/broken_examples/check_options/millproject";
  const string context = "Error parsing configuration file \"" + file + "\": ";
  auto checked = check_options("pcb2gcode --check-options --config=" + file);
  BOOST_CHECK_EQUAL(checked.first, ERR_INVALIDPARAMETER);
  // zchange, after the line that can't be parsed, is read, so it isn't missing.
  const json& errors = checked.second["errors"];
  BOOST_REQUIRE_EQUAL(errors.size(), 3U);
  BOOST_CHECK_EQUAL(errors[0], error_json({"no-such-option"}, "unknown_option",
                                          context + "unrecognised option 'no-such-option'",
                                          ERR_INVALIDPARAMETER));
  BOOST_CHECK_EQUAL(errors[1], error_json({}, "config_file",
                                          context + "the options configuration file contains an "
                                          "invalid line 'this line has no equals sign'",
                                          ERR_INVALIDPARAMETER));
  BOOST_CHECK_EQUAL(errors[2]["options"], json::array({"zsafe"}));
  BOOST_CHECK_EQUAL(errors[2]["rule"], "invalid_value");
  BOOST_CHECK(boost::starts_with(errors[2]["message"].get<string>(), context));
  // A run stops at the first one.
  const auto run = get_check_error("pcb2gcode --config=" + file);
  BOOST_CHECK_EQUAL(run.first, ERR_INVALIDPARAMETER);
  BOOST_CHECK_EQUAL(errors[0]["message"], run.second);

  // Like in a run, the command line wins over the file.
  checked = check_options("pcb2gcode --check-options --zsafe=5mm --config=" + file);
  BOOST_REQUIRE_EQUAL(checked.second["errors"].size(), 2U);
  BOOST_CHECK_EQUAL(checked.second["errors"][1]["rule"], "config_file");

  checked = check_options("pcb2gcode --check-options --zsafe=5mm --zchange=10mm "
                          "--config=no_such_file");
  BOOST_CHECK_EQUAL(checked.second["errors"],
                    json::array({error_json({"config"}, "config_file",
                                            "Can't open the configuration file \"no_such_file\" "
                                            "given with --config.", ERR_INVALIDPARAMETER)}));
}

BOOST_AUTO_TEST_CASE(check_options_reads_config_syntax) {
  const string file = "tests/data/broken_examples/check_options/syntax";
  const string context = "Error parsing configuration file \"" + file + "\": ";
  const auto checked = check_options("pcb2gcode --check-options --config=" + file);
  BOOST_CHECK_EQUAL(checked.first, ERR_INVALIDPARAMETER);
  const string bad_line =
      po::invalid_config_file_syntax("[unterminated", po::invalid_syntax::unrecognized_line).what();
  // Options after a [section] start with its name.
  BOOST_CHECK_EQUAL(
      checked.second["errors"],
      json::array({error_json({}, "unknown_option", context + po::unknown_option("").what(),
                              ERR_INVALIDPARAMETER),
                   error_json({}, "config_file", context + bad_line, ERR_INVALIDPARAMETER),
                   error_json({"an-unknown-option-with-a-long-name"}, "unknown_option",
                              context + "unrecognised option 'an-unknown-option-with-a-long-name'",
                              ERR_INVALIDPARAMETER),
                   error_json({"section.zchange"}, "unknown_option",
                              context + "unrecognised option 'section.zchange'",
                              ERR_INVALIDPARAMETER),
                   error_json({"a-section-with-a-long-name.zwork"}, "unknown_option",
                              context + "unrecognised option 'a-section-with-a-long-name.zwork'",
                              ERR_INVALIDPARAMETER),
                   error_json({"zchange"}, "required:zchange",
                              "Missing --zchange, the tool change height. For example: "
                              "--zchange=10mm", ERR_NOZCHANGE)}));
  const auto run = get_check_error("pcb2gcode --config=" + file);
  BOOST_CHECK_EQUAL(run.first, ERR_INVALIDPARAMETER);
  BOOST_CHECK_EQUAL(checked.second["errors"][0]["message"], run.second);
}

BOOST_AUTO_TEST_CASE(check_options_g64_and_tolerance) {
  const string args = "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm --g64=0.001 "
                      "--tolerance=0.001";
  const auto checked = check_options(args + " --check-options");
  BOOST_CHECK_EQUAL(checked.first, ERR_BOTHTOLERANCEG64);
  BOOST_CHECK_EQUAL(checked.second["errors"],
                    json::array({error_json({"g64", "tolerance"}, "g64_and_tolerance",
                                            "--g64 and --tolerance can't both be set. --g64 is "
                                            "deprecated, so use only --tolerance.",
                                            ERR_BOTHTOLERANCEG64)}));
  BOOST_CHECK_EQUAL(checked.second["warnings"],
                    json::array({{{"options", {"g64"}},
                                  {"message", "g64 is deprecated, use tolerance."}}}));
  BOOST_CHECK_EQUAL(get_error_code(args), ERR_BOTHTOLERANCEG64);
}

BOOST_AUTO_TEST_CASE(check_options_lists_warnings) {
  auto checked = check_options("pcb2gcode --check-options --noconfigfile --zsafe=5mm "
                               "--zchange=10mm --tolerance=1 --svg=x --drill=board.drl "
                               "--drill-front --zdrill=-1mm --drill-feed=1 --drill-speed=1");
  BOOST_CHECK_EQUAL(checked.first, ERR_OK);
  BOOST_CHECK_EQUAL(checked.second["errors"], json::array());
  std::vector<json> warned;
  for (const auto& warning : checked.second["warnings"]) {
    warned.push_back(warning["options"]);
  }
  BOOST_CHECK(warned == (std::vector<json>{{"tolerance"}, {"svg"},
                                           {"drill", "front", "back", "outline"},
                                           {"drill-front"}}));
  // The outline gives the size of the board.
  checked = check_options("pcb2gcode --check-options --noconfigfile --zsafe=5mm --zchange=10mm "
                          "--drill=board.drl --zdrill=-1mm --drill-feed=1 --drill-speed=1 "
                          "--outline=outline.gbr --zcut=-1mm --cutter-diameter=1mm --cut-feed=1 "
                          "--cut-speed=1 --cut-infeed=1mm");
  BOOST_CHECK_EQUAL(checked.first, ERR_OK);
  BOOST_CHECK_EQUAL(checked.second["warnings"], json::array());
}

BOOST_AUTO_TEST_CASE(check_options_with_manifest) {
  const string args = "pcb2gcode --noconfigfile --zsafe=5mm --zchange=10mm --manifest=a.json";
  auto checked = check_options(args + " --check-options");
  BOOST_CHECK_EQUAL(checked.first, ERR_OK);
  BOOST_CHECK_EQUAL(checked.second["errors"], json::array());
  checked = check_options(args + " --check-options --manifest=b.json");
  BOOST_CHECK_EQUAL(checked.first, ERR_UNKNOWNPARAMETER);
  BOOST_CHECK_EQUAL(checked.second["errors"],
                    json::array({error_json({"manifest"}, "invalid_value",
                                            "option '--manifest' cannot be specified more than "
                                            "once", ERR_UNKNOWNPARAMETER)}));
  BOOST_CHECK_EQUAL(get_error_code(args + " --manifest=b.json"), ERR_UNKNOWNPARAMETER);
}

// The exit code of a run with the words of args, and what it prints to stderr
// while it reads and checks the options.
std::pair<ErrorCodes, string> run_check(const string& args) {
  std::stringstream printed;
  struct RestoreCerr {
    std::streambuf* buffer;
    ~RestoreCerr() { cerr.rdbuf(buffer); }
  } restore{cerr.rdbuf(printed.rdbuf())};
  const ErrorCodes code = get_check_error(args).first;
  return {code, printed.str()};
}

// The output of --check-options with args, after checking that a run with args
// stops at its first error and prints each error that --ignore-warnings lets
// it go on past.
json check_like_a_run(const string& args) {
  const auto run = run_check(args);
  const auto checked = check_options(args + " --check-options");
  BOOST_CHECK_EQUAL(checked.first, run.first);
  const json& errors = checked.second["errors"];
  if (!errors.empty()) {
    BOOST_CHECK_NE(run.second.find(errors[0]["message"].get<string>()), string::npos);
  }
  for (const auto& warning : checked.second["warnings"]) {
    if (warning.contains("code")) {
      BOOST_CHECK_NE(run.second.find("Ignoring error code " +
                                     std::to_string(warning["code"].get<int>()) + ": "),
                     string::npos);
      BOOST_CHECK_NE(run.second.find(warning["message"].get<string>()), string::npos);
    }
  }
  return checked.second;
}

std::vector<json> rules_and_options(const json& problems) {
  std::vector<json> found;
  for (const auto& problem : problems) {
    found.push_back({problem["rule"], problem["options"]});
  }
  return found;
}

BOOST_AUTO_TEST_CASE(check_options_with_ignore_warnings) {
  // A run with a file to mill needs these values.
  json checked = check_like_a_run("pcb2gcode --noconfigfile --ignore-warnings --back=board.gbr");
  BOOST_CHECK_EQUAL(checked["errors"].size(), 5U);
  BOOST_CHECK_EQUAL(checked["warnings"], json::array());
  // Without files, nothing needs them, so a run goes on past them.
  checked = check_like_a_run("pcb2gcode --noconfigfile --ignore-warnings");
  BOOST_CHECK_EQUAL(checked["errors"], json::array());
  BOOST_CHECK(rules_and_options(checked["warnings"]) ==
              (std::vector<json>{{"required:zsafe", {"zsafe"}},
                                 {"required:zchange", {"zchange"}}}));
  const string milling = "pcb2gcode --noconfigfile --ignore-warnings --back=board.gbr "
                         "--zsafe=-1mm --zchange=10mm --zwork=-0.1mm --mill-feed=100mm/min "
                         "--mill-speed=10000rpm --tile-x=0";
  checked = check_like_a_run(milling);
  BOOST_CHECK_EQUAL(checked["errors"], json::array());
  BOOST_CHECK(rules_and_options(checked["warnings"]) ==
              (std::vector<json>{{"range:tile-x", {"tile-x"}},
                                 {"greater:zsafe,zwork", {"zsafe", "zwork"}}}));
  // The autoleveller's values are needed only for a layer that it levels.
  BOOST_CHECK_EQUAL(check_like_a_run(milling + " --al-front")["errors"], json::array());
  BOOST_CHECK_EQUAL(check_like_a_run(milling + " --al-back")["errors"][0]["rule"],
                    "required:software");
  // A run stops at a problem on the command line anyway.
  checked = check_options("pcb2gcode --check-options --noconfigfile --ignore-warnings --zsafe=x")
                .second;
  BOOST_CHECK_EQUAL(checked["errors"][0]["rule"], "invalid_value");
  BOOST_CHECK_EQUAL(get_error_code("pcb2gcode --noconfigfile --ignore-warnings --zsafe=x"),
                    ERR_UNKNOWNPARAMETER);
}

BOOST_AUTO_TEST_CASE(check_options_with_unreadable_config_values) {
  const string config = " --config=tests/data/broken_examples/unreadable_value/millproject";
  const std::vector<json> unreadable{{"invalid_value", {"mill-feed-direction"}},
                                     {"invalid_value", {"mill-feed"}},
                                     {"invalid_value", {"mill-speed"}}};
  // A run stops at the first value that it can't read.
  json checked = check_like_a_run("pcb2gcode" + config);
  BOOST_CHECK(rules_and_options(checked["errors"]) == unreadable);
  // With --ignore-warnings, it goes on without them: the options keep their
  // defaults, a repeated option keeps its first value, and the run stops at
  // --mill-feed, which the milling needs.
  checked = check_like_a_run("pcb2gcode --ignore-warnings" + config);
  BOOST_CHECK_EQUAL(checked["errors"],
                    json::array({error_json({"mill-feed"}, "required:mill-feed",
                                            "Missing --mill-feed, the milling feed rate. For "
                                            "example: --mill-feed=600mm/min", ERR_NOMILLFEED)}));
  BOOST_CHECK(rules_and_options(checked["warnings"]) == unreadable);
  BOOST_CHECK_EQUAL(options::get_vm()["mill-speed"].as<Rpm>().asRpm(1), 10000);
  BOOST_CHECK_EQUAL(check_like_a_run("pcb2gcode --ignore-warnings --mill-feed=100mm/min" +
                                     config)["errors"], json::array());
  // A file that Boost can't parse is left out, after its first problem.
  checked = check_like_a_run("pcb2gcode --ignore-warnings "
                             "--config=tests/data/broken_examples/check_options/syntax");
  BOOST_CHECK_EQUAL(checked["errors"], json::array());
  BOOST_CHECK(rules_and_options(checked["warnings"]) ==
              (std::vector<json>{{"unknown_option", json::array()},
                                 {"required:zsafe", {"zsafe"}},
                                 {"required:zchange", {"zchange"}}}));
}

BOOST_AUTO_TEST_SUITE_END()
