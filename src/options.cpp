/*
 * This file is part of pcb2gcode.
 *
 * Copyright (C) 2009, 2010 Patrick Birnzain <pbirnzain@users.sourceforge.net>
 * Copyright (C) 2010 Bernhard Kubicek <kubicek@gmx.at>
 * Copyright (C) 2013 Erik Schuster <erik@muenchen-ist-toll.de>
 * Copyright (C) 2014-2017 Nicola Corna <nicola@corna.info>
 *
 * pcb2gcode is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pcb2gcode is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pcb2gcode.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "options.hpp"
#include "option_metadata.hpp"
#include "pcb2gcode_version.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <list>
#include <map>
#include <sstream>
#include <boost/exception/all.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/variant.hpp>
#include "units.hpp"
#include "available_drills.hpp"

#include <string>
using std::string;

#include <iostream>
using std::cerr;
using std::endl;

namespace {

// The conditions that decide which options pcb2gcode uses and checks.
const std::vector<NamedCondition>& option_conditions() {
  const auto set = &OptionCondition::set;
  const auto equal = &OptionCondition::equal;
  const OptionCondition milldrilling =
      set("drill") && OptionCondition::finite("min-milldrill-hole-diameter");
  static const std::vector<NamedCondition> conditions{
    {"always", "Always.", OptionCondition()},
    {"milling", "A front or back gerber file is given.", set("front") || set("back")},
    {"drilling", "A drill file is given.", set("drill")},
    {"milldrilling", "A drill file is given and --min-milldrill-hole-diameter isn't inf, so "
     "holes at least that wide are milled.", milldrilling},
    {"cutting", "An outline is given, or holes are milldrilled.", set("outline") || milldrilling},
    {"autolevelling", "The autoleveller is on for a side.",
     equal("al-front", "true") || equal("al-back", "true")},
  };
  return conditions;
}

// What --dump-options says about options beyond their declarations.
const std::map<string, OptionDetails>& option_details() {
  const std::map<string, string> sides{{"true", "front"}, {"false", "back"}};
  const string auto_side = "auto: the front, unless only --back is given";
  static const std::map<string, OptionDetails> details{
    {"config", OptionDetails().input("millproject")},
    {"g64", OptionDetails().replaced("tolerance")},
    {"tolerance", OptionDetails().note("0.0004in, or --g64 if it's given")},
    {"output-dir", OptionDetails().directory().note("empty: the working directory")},
    {"manifest", OptionDetails().output("json")},
    {"preamble-text", OptionDetails().input("text")},
    {"preamble", OptionDetails().input("gcode")},
    {"postamble", OptionDetails().input("gcode")},
    {"drill", OptionDetails().input("excellon").when("always")},
    {"milldrill", OptionDetails().replaced("min-milldrill-hole-diameter", {{"true", "0"}})},
    {"milldrill-diameter", OptionDetails().when("milldrilling").note("--cutter-diameter")},
    {"min-milldrill-hole-diameter", OptionDetails().note("inf: no hole is milldrilled")},
    {"min-milldrill-entry-diameter", OptionDetails().when("milldrilling")},
    {"max-milldrill-entry-diameter", OptionDetails().when("milldrilling")},
    {"milldrill-stepover", OptionDetails().when("milldrilling")},
    {"zmilldrill", OptionDetails().when("milldrilling").note("--zdrill")},
    {"drill-front", OptionDetails().replaced("drill-side", sides)},
    {"drill-side", OptionDetails().note(auto_side)},
    {"drill-output",
     OptionDetails().output("gcode").note("with --basename: <basename>_drill.ngc")},
    {"nom6", OptionDetails().when("always")},
    {"milldrill-output", OptionDetails().output("gcode").when("milldrilling")
                             .note("with --basename: <basename>_milldrill.ngc")},
    {"front", OptionDetails().input("gerber").when("always")},
    {"back", OptionDetails().input("gerber").when("always")},
    // extra-passes has no replacement: the --isolation-width that matches a
    // number of passes depends on the tool diameter and the overlap.
    {"mill-vertfeed", OptionDetails().note("half of --mill-feed")},
    {"mill-infeed", OptionDetails().note("the whole --zwork depth in one pass")},
    {"front-output",
     OptionDetails().output("gcode").note("with --basename: <basename>_front.ngc")},
    {"back-output",
     OptionDetails().output("gcode").note("with --basename: <basename>_back.ngc")},
    {"outline", OptionDetails().input("gerber").when("always")},
    {"cut-vertfeed", OptionDetails().note("half of --cut-feed")},
    {"cut-front", OptionDetails().replaced("cut-side", sides)},
    {"cut-side", OptionDetails().note(auto_side)},
    {"bridgesnum", OptionDetails().note("no bridges unless --bridges is more than 0")},
    {"zbridges", OptionDetails().note("--zsafe")},
    {"outline-output",
     OptionDetails().output("gcode").note("with --basename: <basename>_outline.ngc")},
    {"backtrack", OptionDetails().note("inf: retrace a milled path whenever that's faster")},
    {"al-front", OptionDetails().when("always")},
    {"al-back", OptionDetails().when("always")},
    // Tiling uses it too.
    {"software", OptionDetails().when("always").note("custom, for tiling")},
    {"spindown-time", OptionDetails().note("--spinup-time")},
  };
  return details;
}

// The checks that check_parameters() runs, in order.  pcb2gcode reports every
// failure and exits with the code of the first.
const std::vector<OptionRule>& option_rules() {
  const auto required = &OptionRule::required;
  const auto range = &OptionRule::range;
  const auto greater = &OptionRule::greater;
  const auto forbidden = &OptionRule::forbidden;
  const auto equal = &OptionCondition::equal;
  const auto set = &OptionCondition::set;
  const auto given = &OptionCondition::given;
  // Every mill and the driller use zsafe and zchange.
  const OptionCondition processing = set("front") || set("back") || set("outline") || set("drill");
  // The autoleveller reads its values only for a side that has a layer.
  const OptionCondition autolevelled = (equal("al-front", "true") && set("front")) ||
                                       (equal("al-back", "true") && set("back"));
  static const std::vector<OptionRule> rules{
    range("spinup-time").at_least("0")
        .fails_with(ERR_NEGATIVESPINUP, "--spinup-time can't be negative."),
    range("spindown-time").at_least("0")
        .fails_with(ERR_NEGATIVESPINDOWN, "--spindown-time can't be negative."),
    forbidden(!equal("mirror-absolute", "true"), {"mirror-absolute"})
        .fails_with(ERR_FALSEMIRRORABSOLUTE,
                    "--mirror-absolute=false isn't supported anymore. Remove it, and use "
                    "--mirror-axis to choose where the back side is mirrored."),
    range("tolerance").above("0")
        .fails_with(ERR_NEGATIVETOLERANCE,
                    "--tolerance must be greater than 0, because arcs are cut as straight lines "
                    "that stay within the tolerance of the arc. Leave it out to use the default."),
    range("tile-x").at_least("1").fails_with(ERR_NEGATIVETILEX, "--tile-x must be at least 1."),
    range("tile-y").at_least("1").fails_with(ERR_NEGATIVETILEY, "--tile-y must be at least 1."),
    required("zsafe", "the safety height for rapid moves", "5mm")
        .needed_when(processing).fails_with(ERR_NOZSAFE),
    required("zchange", "the tool change height", "10mm")
        .needed_when(processing).fails_with(ERR_NOZCHANGE),
    required("software", "the autoleveller's target software: linuxcnc, mach3, mach4 or custom",
             "linuxcnc")
        .when("autolevelling").needed_when(autolevelled).fails_with(ERR_NOSOFTWARE),
    required("al-x", "the autoleveller's largest x distance between probes", "15mm")
        .when("autolevelling").needed_when(autolevelled).fails_with(ERR_NOALX),
    range("al-x").above("0").when("autolevelling")
        .fails_with(ERR_NEGATIVEALX, "--al-x must be greater than 0."),
    required("al-y", "the autoleveller's largest y distance between probes", "15mm")
        .when("autolevelling").needed_when(autolevelled).fails_with(ERR_NOALY),
    range("al-y").above("0").when("autolevelling")
        .fails_with(ERR_NEGATIVEALY, "--al-y must be greater than 0."),
    required("al-probefeed", "the autoleveller's probing feed rate", "100mm/min")
        .when("autolevelling").needed_when(autolevelled).fails_with(ERR_NOALPROBEFEED),
    range("al-probefeed").above("0").when("autolevelling")
        .fails_with(ERR_NEGATIVEPROBEFEED, "--al-probefeed must be greater than 0."),
    forbidden(!equal("mill-feed-direction", "any") && equal("tsp-2opt", "true"),
              {"mill-feed-direction", "tsp-2opt"})
        .fails_with(ERR_INVALIDPARAMETER,
                    "--mill-feed-direction doesn't work with --tsp-2opt, which is on by "
                    "default. Add --tsp-2opt=false."),

    required("zwork", "the milling depth", "-0.05mm").when("milling").fails_with(ERR_NOZWORK),
    forbidden(!equal("vectorial", "true"), {"vectorial"}).when("milling")
        .fails_with(ERR_INVALIDPARAMETER, "--vectorial=false isn't supported anymore. Remove it."),
    required("mill-diameters", "the diameters of the milling bits", "0.2mm")
        .when("milling").fails_with(ERR_NOOFFSET),
    required("mill-feed", "the milling feed rate", "600mm/min")
        .when("milling").fails_with(ERR_NOMILLFEED),
    required("mill-speed", "the spindle speed for milling", "10000rpm")
        .when("milling").fails_with(ERR_NOMILLSPEED),
    greater("zsafe", "zwork").when("milling")
        .fails_with(ERR_ZSAFELOWERZWORK, "--zsafe must be higher than --zwork."),
    range("mill-feed").above("0").when("milling")
        .fails_with(ERR_NEGATIVEMILLFEED, "--mill-feed must be greater than 0."),
    range("mill-vertfeed").above("0").when("milling")
        .fails_with(ERR_NEGATIVEMILLVERTFEED, "--mill-vertfeed must be greater than 0."),
    range("mill-infeed").above("0").when("milling")
        .fails_with(ERR_LOWMILLINFEED, "--mill-infeed must be greater than 0."),
    range("mill-speed").at_least("0").when("milling")
        .fails_with(ERR_NEGATIVEMILLSPEED, "--mill-speed can't be negative."),

    required("zcut", "the depth for cutting the outline and milldrilling", "-2.5mm")
        .when("cutting").fails_with(ERR_NOZCUT),
    range("zcut").at_most("0").when("cutting")
        .fails_with(ERR_NEGATIVEZWORK, "--zcut can't be above 0, the surface of the board."),
    required("cutter-diameter", "the end mill diameter for cutting the outline and milldrilling",
             "3mm")
        .when("cutting").fails_with(ERR_NOCUTTERDIAMETER),
    required("cut-feed", "the feed rate for cutting the outline and milldrilling", "600mm/min")
        .when("cutting").fails_with(ERR_NOCUTFEED),
    required("cut-speed", "the spindle speed for cutting the outline and milldrilling",
             "10000rpm")
        .when("cutting").fails_with(ERR_NOCUTSPEED),
    required("cut-infeed",
             "the maximum depth of each pass when cutting the outline and milldrilling", "1mm")
        .when("cutting").fails_with(ERR_NOCUTINFEED),
    greater("zsafe", "zcut").when("cutting")
        .fails_with(ERR_ZSAFELOWERZCUT, "--zsafe must be higher than --zcut."),
    range("cut-feed").above("0").when("cutting")
        .fails_with(ERR_NEGATIVECUTFEED, "--cut-feed must be greater than 0."),
    range("cut-vertfeed").above("0").when("cutting")
        .fails_with(ERR_NEGATIVECUTVERTFEED, "--cut-vertfeed must be greater than 0."),
    range("cut-speed").at_least("0").when("cutting")
        .fails_with(ERR_NEGATIVESPINDLESPEED, "--cut-speed can't be negative."),
    range("cut-infeed").at_least("0.001in").when("cutting")
        .fails_with(ERR_LOWCUTINFEED, "--cut-infeed must be at least 0.001in."),
    range("bridges").at_least("0").when("cutting")
        .fails_with(ERR_NEGATIVEBRIDGE, "--bridges can't be negative."),
    forbidden(set("cut-front") && given("cut-side"), {"cut-front", "cut-side"}).when("cutting")
        .fails_with(ERR_BOTHCUTFRONTSIDE, "--cut-front and --cut-side can't both be set. "
                    "--cut-front is deprecated, so use only --cut-side."),

    // A stepover of more than 100% would leave an uncut ring between passes.
    range("milldrill-stepover").above("0").at_most("100%").when("milldrilling")
        .fails_with(ERR_INVALIDMILLDRILLSTEPOVER,
                    "--milldrill-stepover must be greater than 0% and at most 100%."),
    // The cutter can't make a hole smaller than itself.
    range("min-milldrill-entry-diameter").above("100%").when("milldrilling")
        .fails_with(ERR_LOWMILLDRILLENTRY, "--min-milldrill-entry-diameter must be greater than "
                    "100%, which is the milldrill diameter."),
    OptionRule::greater_or_equal("max-milldrill-entry-diameter", "min-milldrill-entry-diameter")
        .when("milldrilling")
        .fails_with(ERR_MILLDRILLENTRYORDER, "--max-milldrill-entry-diameter can't be less than "
                    "--min-milldrill-entry-diameter."),

    required("zdrill", "the drilling depth", "-2.5mm").when("drilling").fails_with(ERR_NOZDRILL),
    greater("zsafe", "zdrill").when("drilling")
        .fails_with(ERR_ZSAFELOWERZDRILL, "--zsafe must be higher than --zdrill."),
    greater("zchange", "zdrill").unless(equal("zchange-absolute", "true")).when("drilling")
        .fails_with(ERR_ZSAFELOWERZCHANGE, "--zchange must be higher than --zdrill. If --zchange "
                    "is a machine coordinate, add --zchange-absolute."),
    required("drill-feed", "the drilling feed rate", "500mm/min")
        .when("drilling").fails_with(ERR_NODRILLFEED),
    range("drill-feed").above("0").when("drilling")
        .fails_with(ERR_NEGATIVEDRILLFEED, "--drill-feed must be greater than 0."),
    required("drill-speed", "the spindle speed for drilling", "10000rpm")
        .when("drilling").fails_with(ERR_NODRILLSPEED),
    range("drill-speed").at_least("0").when("drilling")
        .fails_with(ERR_NEGATIVEDRILLSPEED, "--drill-speed can't be negative."),
    forbidden(set("drill-front") && given("drill-side"), {"drill-front", "drill-side"})
        .when("drilling")
        .fails_with(ERR_BOTHDRILLFRONTSIDE, "--drill-front and --drill-side can't both be set. "
                    "--drill-front is deprecated, so use only --drill-side."),
  };
  return rules;
}

// options_description has no getter for its caption, but it prints it first.
string caption(const po::options_description& description) {
  std::stringstream help;
  help << description;
  string caption;
  std::getline(help, caption);
  if (!caption.empty() && caption.back() == ':') {
    caption.pop_back();
  }
  return caption;
}

std::vector<string> option_names(const po::options_description& description) {
  std::vector<string> names;
  for (const auto& option : description.options()) {
    names.push_back(option->long_name());
  }
  return names;
}

// The errors as pcb2gcode prints them, one per line.
string error_lines(const std::vector<OptionError>& errors) {
  string lines;
  for (const auto& error : errors) {
    lines += "Error: ";
    lines += error.what;
    lines += "\n";
  }
  return lines;
}

// The option that an error of Boost's command line parser is about.  Boost
// names it like --zsafe: a mistake with a short option is an unknown option.
std::vector<string> error_options(const std::logic_error& error) {
  const auto* named = dynamic_cast<const po::error_with_option_name*>(&error);
  return named == nullptr ? std::vector<string>()
                          : std::vector<string>(1, boost::trim_left_copy_if(
                                                       named->get_option_name(),
                                                       boost::is_any_of("-")));
}

/* Adds a problem about option, or about none if it's empty.  The message is
 * context and message, then quoted in quotes unless it's empty, the way Boost
 * quotes names in its messages and leaves out an empty one.
 */
void add_problem(std::vector<OptionError>& problems, ErrorCodes code, const char* rule,
                 const char* option, const string& context, const char* message,
                 const char* quoted = "") {
  problems.push_back({context + message, code, {}, rule});
  OptionError& problem = problems.back();
  if (*quoted != '\0') {
    problem.what += " '";
    problem.what += quoted;
    problem.what += "'";
  }
  if (*option != '\0') {
    problem.options.push_back(option);
  }
}

/* Boost's parser stops at the first unknown option and at the first line that
 * isn't a comment, a [section] or option=value.  While checking, those
 * problems are reported in the order of the lines, with the messages of
 * Boost's unknown_option and invalid_config_file_syntax, and the lines that
 * Boost can't parse are left out of the text returned, so that the others can
 * still be read.
 */
string check_config_lines(std::istream& stream, const po::options_description& description,
                          const string& context, std::vector<OptionError>& problems) {
  const auto blank = boost::is_any_of(" \t\r\n");
  string text;
  string prefix;
  string line;
  while (std::getline(stream, line)) {
    string content = line.substr(0, line.find('#'));
    boost::trim_if(content, blank);
    const auto equals = content.find('=');
    if (!content.empty() && content.front() == '[' && content.back() == ']') {
      prefix = content.substr(1, content.size() - 2);
      if (!boost::ends_with(prefix, ".")) {
        prefix += ".";
      }
    } else if (equals != string::npos) {
      string name = content.substr(0, equals);
      boost::trim_if(name, blank);
      name.insert(0, prefix);
      if (name.empty() || description.find_nothrow(name, false) == nullptr) {
        add_problem(problems, ERR_INVALIDPARAMETER, "unknown_option", name.c_str(), context,
                    "unrecognised option", name.c_str());
      }
    } else if (!content.empty()) {
      add_problem(problems, ERR_INVALIDPARAMETER, "config_file", "", context,
                  "the options configuration file contains an invalid line", content.c_str());
      line.clear();
    }
    text += line;
    text += "\n";
  }
  return text;
}

} // namespace

/******************************************************************************/
/*
 */
/******************************************************************************/
options& options::instance() {
    static options singleton;
    return singleton;
}

void options::maybe_throw(const std::string& what, ErrorCodes error_code) {
  if (instance().vm["ignore-warnings"].as<bool>()) {
    cerr << "Ignoring error code " << error_code << ": " << what << endl;
  } else {
    throw pcb2gcode_parse_exception(what, error_code);
  }
}

void options::report(const OptionError& error) {
  options& self = instance();
  if (!self.checking) {
    maybe_throw(error.what, error.code);
    return;
  }
  // A run passes it to maybe_throw().
  std::vector<OptionError>& list =
      self.vm["ignore-warnings"].as<bool>() ? self.ignored : self.problems;
  list.push_back(error);
  // Like the messages of the rules.
  string& what = list.back().what;
  if (boost::starts_with(what, "Error: ")) {
    what.erase(0, std::strlen("Error: "));
  }
}

/* While checking: stores the values that can be read, like store_config_file()
 * does, and reports the others: a second value of an option that takes one
 * isn't read either, and the option keeps the first.  context starts their
 * messages and code is their exit code.  With ignorable, a run with
 * --ignore-warnings goes on without them, like it does in a configuration
 * file.
 */
void options::store_readable(const po::parsed_options& parsed, const string& context,
                             ErrorCodes code, bool ignorable) {
  options& self = instance();
  const bool ignored = ignorable && self.vm["ignore-warnings"].as<bool>();
  std::map<string, boost::any> values;
  std::set<string> unread;
  po::parsed_options readable(parsed.description, parsed.m_options_prefix);
  for (const po::option& option : parsed.options) {
    const string& name = option.string_key;
    if (option.unregistered || name.empty() || self.unreadable.count(name) > 0) {
      continue;
    }
    const po::value_semantic& semantic = *parsed.description->find(name, false).semantic();
    // po::store keeps the values from earlier sources, like the command line.
    if (self.vm.count(name) > 0 && !self.vm[name].defaulted() && !semantic.is_composing()) {
      continue;
    }
    try {
      semantic.parse(values[name], option.value, false);
      readable.options.push_back(option);
    } catch (po::error_with_option_name& e) {
      // The message names the option the way po::store's does.
      e.add_context(name, option.original_tokens.empty() ? name : option.original_tokens[0],
                    parsed.m_options_prefix);
      add_problem(ignored ? self.ignored : self.problems, code, "invalid_value", name.c_str(),
                  context, e.what());
      unread.insert(name);
    }
  }
  po::store(readable, self.vm);
  // A run that stops at a value that it can't read doesn't get to the rules
  // about the option.  With --ignore-warnings, it checks them without the value.
  for (const auto& name : unread) {
    if (!ignored && values[name].empty()) {
      self.unreadable.insert(name);
    }
  }
}

/* While checking: reads a configuration file like parse_files() does, but
 * reports every problem.  Although a run stops at the first problem, the lines
 * that can be read are stored too, unless --ignore-warnings leaves out the
 * whole file like a run does.
 */
void options::check_config_file(std::istream& stream, const string& context) {
  options& self = instance();
  std::vector<OptionError> unparsed;
  std::istringstream readable(check_config_lines(stream, self.cfg_options, context, unparsed));
  if (!unparsed.empty() && self.vm["ignore-warnings"].as<bool>()) {
    // A run reports only the problem that its parser stops at.
    self.ignored.push_back(unparsed.front());
    return;
  }
  self.problems.insert(self.problems.end(), unparsed.cbegin(), unparsed.cend());
  store_readable(po::parse_config_file(readable, self.cfg_options, true), context,
                 ERR_INVALIDPARAMETER, true);
}

/* Adjusts the processed varibles map in place to fix for deprecated variables,
 * etc.
 */
void fix_variables_map(po::variables_map& vm) {
  // Deal with the deprecated milldrill option.
  if (vm["min-milldrill-hole-diameter"].defaulted() && vm["milldrill"].as<bool>()) {
    vm.at("min-milldrill-hole-diameter").value() = Length(0);
  }
  // Deal with deprecated offset option.
  if (vm.count("offset") && vm.at("mill-diameters").defaulted()) {
    vm.at("mill-diameters").as<std::vector<CommaSeparated<Length>>>().clear();
    vm.at("mill-diameters").as<std::vector<CommaSeparated<Length>>>().push_back({vm["offset"].as<Length>()*2.0});
    vm.at("offset").value() = Length(0);
  }

  if (vm["bridgesnum"].as<unsigned int>() > 0 && vm["bridges"].as<Length>().asInch(1) <= 0) {
    vm.at("bridgesnum").value() = (unsigned int) 0;
  }
}

/* parse options, both command line and from the millproject file if it exists.
 * Throws on error.
 */
void options::parse(int argc, const char** argv) {
    // guessing causes problems when one option is the start of another
    // (--drill, --drill-diameter); see bug 3089930
    int style = po::command_line_style::default_style
                & ~po::command_line_style::allow_guessing;

    po::options_description generic;
    generic.add(instance().cli_options).add(instance().cfg_options);

    try {
      po::parsed_options parsed = po::command_line_parser(argc, argv)
          .options(generic).style(style).allow_unregistered().run();
      std::vector<OptionError> unrecognized;
      for (const po::option& option : parsed.options) {
        const string token = boost::algorithm::join(option.original_tokens, " ");
        if (option.position_key != -1) {
          unrecognized.push_back({"Unexpected argument " + token +
                                  ". Arguments need an option name, like --front=board.gbr",
                                  ERR_UNKNOWNPARAMETER, {}, "unexpected_argument"});
        } else if (option.unregistered) {
          const string name = boost::trim_left_copy_if(option.string_key, boost::is_any_of("-"));
          unrecognized.push_back(
              {"Unknown option " + token, ERR_UNKNOWNPARAMETER, {name}, "unknown_option"});
        }
      }
      if (instance().checking) {
        instance().problems.insert(instance().problems.end(), unrecognized.cbegin(),
                                   unrecognized.cend());
        store_readable(parsed, "", ERR_UNKNOWNPARAMETER, false);
      } else {
        if (!unrecognized.empty()) {
          throw pcb2gcode_parse_exception(
              error_lines(unrecognized) + "Run pcb2gcode --help to see all the options.",
              ERR_UNKNOWNPARAMETER);
        }
        po::store(parsed, instance().vm);
      }
    } catch (std::logic_error& e) {
      if (instance().checking) {
        // The rest of the command line can't be read.
        instance().problems.push_back(
            {e.what(), ERR_UNKNOWNPARAMETER, error_options(e), "invalid_value"});
        return;
      }
      throw pcb2gcode_parse_exception(std::string("Error: ") + e.what() +
                                      "\nRun pcb2gcode --help to see all the options.",
                                      ERR_UNKNOWNPARAMETER);
    }

    po::notify(instance().vm);

    if (!instance().vm["noconfigfile"].as<bool>()) {
      parse_files(
          flatten(
              instance().vm["config"].as<std::vector<CommaSeparated<string>>>()),
          instance().vm["config"].defaulted());
    }

    if (instance().vm.count("basename")) {
      /*
       * this needs to be an extra step, as --basename modifies the default
       * values of the --...-output parameters
       */
      string basename = "";
      basename = instance().vm["basename"].as<string>() + "_";
      instance().vm.at("front-output").value() = basename + "front.ngc";
      instance().vm.at("back-output").value() = basename + "back.ngc";
      instance().vm.at("drill-output").value() = basename + "drill.ngc";
      instance().vm.at("outline-output").value() = basename + "outline.ngc";
      instance().vm.at("milldrill-output").value() = basename + "milldrill.ngc";
    }

    if (instance().vm.count("tolerance")) {
      if (instance().vm.count("g64")) {
        report({"Error: --g64 and --tolerance can't both be set. --g64 is deprecated, so use "
                "only --tolerance.", ERR_BOTHTOLERANCEG64, {"g64", "tolerance"},
                "g64_and_tolerance"});
      }
    } else if (instance().vm.count("g64")) {
      instance().vm.insert({"tolerance", po::variable_value(instance().vm["g64"].value(), false)});
    } else {
      // 0.0004in, as a number in --metric units: parse_unit<Length>("0.0004in") is a slightly
      // different number of inches, which would change the G-code.
      const double cfactor = instance().vm["metric"].as<bool>() ? 25.4 : 1;
      instance().vm.insert({"tolerance", po::variable_value(Length(0.0004 * cfactor), true)});
    }
    fix_variables_map(instance().vm);

    po::notify(instance().vm);
}

/******************************************************************************/
/*
 */
/******************************************************************************/
string options::help()
{
    std::stringstream msg;
    msg << PACKAGE_STRING << "\n\n";
    msg << instance().cli_options << instance().cfg_options;
    return msg.str();
}

/* po::store stops at the first value that it can't read and leaves that
 * option without a value, not even its default.  So each option is tried on
 * a copy, with the ones before it that could be read, and with
 * --ignore-warnings the options that can't be read keep their earlier values.
 */
static void store_config_file(const po::parsed_options& parsed, const string& error) {
  po::variables_map& vm = options::get_vm();
  po::parsed_options readable(parsed.description, parsed.m_options_prefix);
  for (const po::option& option : parsed.options) {
    readable.options.push_back(option);
    po::variables_map tried = vm;
    try {
      po::store(readable, tried);
    } catch (std::exception& e) {
      readable.options.pop_back();
      options::maybe_throw(error + e.what(), ERR_INVALIDPARAMETER);
    }
  }
  po::store(readable, vm);
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void options::parse_files(const std::vector<std::string>& config_files,
                          const bool defaulted) {
  // We do it in serve order so that the latter config file in the
  // list overrides the former.
  for (const auto& file : boost::adaptors::reverse(config_files)) {
    std::ifstream stream(file);
    if (!defaulted && stream.fail()) {
      // If the user explicitly specified the config files to use
      // then we will throw an error if any of those files are not
      // found.
      report({"Error: Can't open the configuration file \"" + file + "\" given with --config.",
              ERR_INVALIDPARAMETER, {"config"}, "config_file"});
    }
    const string error = "Error parsing configuration file \"" + file + "\": ";
    if (instance().checking) {
      check_config_file(stream, error);
      continue;
    }
    po::parsed_options parsed(&instance().cfg_options);
    try {
      parsed = po::parse_config_file(stream, instance().cfg_options);
    } catch (std::exception& e) {
      maybe_throw(error + e.what(), ERR_INVALIDPARAMETER);
      continue;
    }
    store_config_file(parsed, error);
  }
  po::notify(instance().vm);
}

/******************************************************************************/
/*
 */
/******************************************************************************/
options::options()
         : cli_options("command line only options"), cfg_options("Generic options (CLI and config files)") {

   cli_options.add_options()
       ("noconfigfile", po::value<bool>()->default_value(false)->implicit_value(true), "ignore any configuration file")
       ("config",
        po::value<std::vector<CommaSeparated<string>>>()->
        default_value(std::vector<CommaSeparated<string>>{{"millproject"}})->multitoken(),
        "list of comma-separated config files")
       ("help,?", "produce help message")
       ("version,V", "show the current software version")
       ("mcp", "run as a Model Context Protocol (MCP) server over stdio")
       ("dump-options", "print every option with its type, default, limits and description as JSON")
       ("check-options", "check the options and print every problem as JSON, without reading input "
        "files or writing output");

   po::options_description drilling_options("Drilling options, for making holes in the PCB");
   drilling_options.add_options()
       ("drill", po::value<string>(), "Excellon drill file")
       ("milldrill", po::value<bool>()->default_value(false)->implicit_value(true), "[DEPRECATED] Use min-milldrill-hole-diameter=0 instead")
       ("milldrill-diameter", po::value<Length>(), "diameter of the end mill used for drilling with --milldrill")
       ("min-milldrill-hole-diameter", po::value<Length>()->default_value(Length(std::numeric_limits<double>::infinity())),
        "minimum hole width for milldrilling.  Holes smaller than this are drilled.  This implies milldrill")
       ("min-milldrill-entry-diameter",
        po::value<Percent>()->default_value(parse_unit<Percent>("150%")),
        "minimum initial hole width for milldrilling when multiple steps are used, as a fraction of milldrill-diameter")
       ("max-milldrill-entry-diameter",
        po::value<Percent>()->default_value(parse_unit<Percent>("190%")),
        "maximum initial hole width for milldrilling in a single step, and the minimum size of the initial hole"
        " when multiple steps are used, as a fraction of milldrill-diameter")
       ("milldrill-stepover",
        po::value<Percent>()->default_value(parse_unit<Percent>("50%")),
        "maximum hole radius increase when milldrilling in multiple steps, as a fraction of milldrill-diameter")
       ("zdrill", po::value<Length>(), "drilling depth")
       ("zmilldrill", po::value<Length>(), "milldrilling depth")
       ("drill-feed", po::value<Velocity>(), "drill feed in [i/m] or [mm/m]")
       ("drill-speed", po::value<Rpm>(), "spindle rpm when drilling")
       ("drill-front", po::value<bool>()->implicit_value(true), "[DEPRECATED, use drill-side instead] drill through the front side of board")
       ("drill-side", po::value<BoardSide::BoardSide>()->default_value(BoardSide::AUTO),
        "drill side; valid choices are front, back or auto (default)")
       ("drills-available", po::value<std::vector<AvailableDrills>>()
        ->default_value(std::vector<AvailableDrills>{})
        ->multitoken(), "list of drills available")
       ("onedrill", po::value<bool>()->default_value(false)->implicit_value(true), "use only one drill bit size")
       ("drill-output", po::value<string>()->default_value("drill.ngc"), "output file for drilling")
       ("nog91-1", po::value<bool>()->default_value(false)->implicit_value(true), "do not explicitly set G91.1 in drill headers")
       ("nog81", po::value<bool>()->default_value(false)->implicit_value(true), "replace G81 with G0+G1")
       ("nom6", po::value<bool>()->default_value(false)->implicit_value(true), "do not emit M6 on tool changes")
       ("milldrill-output", po::value<string>()->default_value("milldrill.ngc"), "output file for milldrilling");
   add_group("drilling", "drilling", drilling_options);

   po::options_description milling_options("Milling options, for milling traces into the PCB");
   milling_options.add_options()
       ("front", po::value<string>(),"front side RS274-X .gbr")
       ("back", po::value<string>(), "back side RS274-X .gbr")
       ("voronoi", po::value<bool>()->default_value(false)->implicit_value(true), "generate voronoi regions")
       ("offset", po::value<Length>()->default_value(0), "Note: Prefer to use --mill-diameters and --milling-overlap if you just that's what you mean."
        "  An optional offset to add to all traces, useful if the bit has a little slop that you want to keep out of the trace.")
       ("mill-diameters", po::value<std::vector<CommaSeparated<Length>>>()->default_value({{Length(0)}})
        ->multitoken(), "Diameters of mill bits, used in the order that they are provided.")
       ("milling-overlap", po::value<boost::variant<Length, Percent>>()->default_value(parse_unit<Percent>("50%")),
        "How much to overlap milling passes, from 0% to 100% or an absolute length")
       ("isolation-width", po::value<Length>()->default_value(Length(0)),
        "Minimum isolation width between copper surfaces")
       ("extra-passes", po::value<int>()->default_value(0), "[DEPRECATED] use --isolation-width instead. "
        "Specify the the number of extra isolation passes, increasing the isolation width half the tool diameter with each pass")
       ("pre-milling-gcode", po::value<std::vector<string>>()->default_value(std::vector<string>{}, ""),
        "custom gcode inserted before the start of milling each trace (used to activate pump or fan or laser connected to fan)")
       ("post-milling-gcode", po::value<std::vector<string>>()->default_value(std::vector<string>{}, ""),
        "custom gcode inserted after the end of milling each trace (used to deactivate pump or fan or laser connected to fan)")
       ("zwork", po::value<Length>(), "milling depth in inches (Z-coordinate while engraving)")
       ("mill-feed", po::value<Velocity>(), "feed while isolating in [i/m] or [mm/m]")
       ("mill-vertfeed", po::value<Velocity>(), "vertical feed while isolating in [i/m] or [mm/m]")
       ("mill-infeed", po::value<Length>(), "maximum milling depth; PCB may be cut in multiple passes")
       ("mill-speed", po::value<Rpm>(), "spindle rpm when milling")
       ("mill-feed-direction", po::value<MillFeedDirection::MillFeedDirection>()->default_value(MillFeedDirection::ANY),
        "In which direction should all milling occur")
       ("invert-gerbers", po::value<bool>()->default_value(false)->implicit_value(true),
        "Invert polarity of front and back gerbers, causing the milling to occur inside the shapes")
       ("draw-gerber-lines", po::value<bool>()->default_value(false)->implicit_value(true),
        "Draw lines in the gerber file as just lines and not as filled in shapes")
       ("preserve-thermal-reliefs", po::value<bool>()->default_value(true)->implicit_value(true), "generate mill paths for thermal reliefs in voronoi mode")
       ("front-output", po::value<string>()->default_value("front.ngc"), "output file for front layer")
       ("back-output", po::value<string>()->default_value("back.ngc"), "output file for back layer");
   add_group("milling", "milling", milling_options);

   po::options_description outline_options("Outline options, for cutting the PCB out of the FR4");
   outline_options.add_options()
       ("outline", po::value<string>(), "pcb outline polygon RS274-X .gbr")
       ("fill-outline", po::value<bool>()->default_value(true)->implicit_value(true), "accept a contour instead of a polygon as outline (enabled by default)")
       ("cutter-diameter", po::value<Length>(), "diameter of the end mill used for cutting out the PCB")
       ("zcut", po::value<Length>(), "PCB cutting depth in inches")
       ("cut-feed", po::value<Velocity>(), "PCB cutting feed in [i/m] or [mm/m]")
       ("cut-vertfeed", po::value<Velocity>(), "PCB vertical cutting feed in [i/m] or [mm/m]")
       ("cut-speed", po::value<Rpm>(), "spindle rpm when cutting")
       ("cut-infeed", po::value<Length>(), "maximum cutting depth; PCB may be cut in multiple passes")
       ("cut-front", po::value<bool>()->implicit_value(true), "[DEPRECATED, use cut-side instead] cut from front side. ")
       ("cut-side", po::value<BoardSide::BoardSide>()->default_value(BoardSide::AUTO), "cut side; valid choices are front, back or auto (default)")
       ("bridges", po::value<Length>()->default_value(Length(0)), "add bridges with the given width to the outline cut")
       ("bridgesnum", po::value<unsigned int>()->default_value(2), "specify how many bridges should be created")
       ("zbridges", po::value<Length>(), "bridges height (Z-coordinates while engraving bridges, default to zsafe) ")
       ("outline-output", po::value<string>()->default_value("outline.ngc"), "output file for outline");
   add_group("outline", "cutting", outline_options);

   po::options_description optimization_options("Optimization options, for faster PCB creation, smaller output files, and different algorithms.");
   optimization_options.add_options()
       ("optimise", po::value<Length>()->default_value(parse_unit<Length>("0.0001in"))->implicit_value(parse_unit<Length>("0.0001in")),
        "Reduce output file size by up to 40% while accepting a little loss of precision.  Larger values reduce file sizes and processing time even further.  Set to 0 to disable.")
       ("eulerian-paths", po::value<bool>()->default_value(true)->implicit_value(true), "Don't mill the same path twice if milling loops overlap.  This can save up to 50% of milling time.  Enabled by default.")
       ("vectorial", po::value<bool>()->default_value(true)->implicit_value(true), "[DEPRECATED, must always be true] enable or disable the vectorial rendering engine")
       ("tsp-2opt", po::value<bool>()->default_value(true)->implicit_value(true), "use TSP 2OPT to find a faster toolpath (but slows down gcode generation)")
       ("path-finding-limit", po::value<size_t>()->default_value(1), "Use path finding for up to this many steps in the search (more is slower but makes a faster gcode path)")
       ("g0-vertical-speed", po::value<Velocity>()->default_value(parse_unit<Velocity>("50in/min")), "speed of vertical G0 movements, for use in path-finding")
       ("g0-horizontal-speed", po::value<Velocity>()->default_value(parse_unit<Velocity>("100in/min")), "speed of horizontal G0 movements, for use in path-finding")
       ("backtrack", po::value<Velocity>()->default_value(std::numeric_limits<double>::infinity()), "allow retracing a milled path if it's faster than retract-move-lower.  For example, set to 5in/s if you are willing to remill 5 inches of trace in order to save 1 second of milling time.");
   add_group("optimization", "always", optimization_options);

   po::options_description autolevelling_options("Autolevelling options, for generating gcode to automatically probe the board and adjust milling depth to the actual board height");
   autolevelling_options.add_options()
       ("al-front", po::value<bool>()->default_value(false)->implicit_value(true), "enable the z autoleveller for the front layer")
       ("al-back", po::value<bool>()->default_value(false)->implicit_value(true),
        "enable the z autoleveller for the back layer")
       ("software", po::value<Software::Software>(),
        "choose the destination software (useful only with the autoleveller). Supported programs are linuxcnc, mach3, mach4 and custom")
       ("al-x", po::value<Length>(), "max x distance between probes")
       ("al-y", po::value<Length>(), "max y distance bewteen probes")
       ("al-probefeed", po::value<Velocity>(), "speed during the probing")
       ("al-probe-on", po::value<string>()->default_value("(MSG, Attach the probe tool)@M0 ( Temporary machine stop. )"),
        "execute this commands to enable the probe tool (default is M0)")
       ("al-probe-off", po::value<string>()->default_value("(MSG, Detach the probe tool)@M0 ( Temporary machine stop. )"),
        "execute this commands to disable the probe tool (default is M0)")
       ("al-probecode", po::value<string>()->default_value("G31"), "custom probe code (default is G31)")
       ("al-probevar", po::value<unsigned int>()->default_value(2002), "number of the variable where the result of the probing is saved (default is 2002)")
       ("al-setzzero", po::value<string>()->default_value("G92 Z0"), "gcode for setting the actual position as zero (default is G92 Z0)");
   add_group("autolevelling", "autolevelling", autolevelling_options);

   po::options_description alignment_options("Alignment options, useful for aligning the milling on opposite sides of the PCB");
   alignment_options.add_options()
       ("x-offset", po::value<Length>()->default_value(0), "offset the origin in the x-axis by this length")
       ("y-offset", po::value<Length>()->default_value(0), "offset the origin in the y-axis by this length")
       ("zero-start", po::value<bool>()->default_value(false)->implicit_value(true), "set the starting point of the project at (0,0)")
       ("mirror-absolute", po::value<bool>()->default_value(true)->implicit_value(true),
        "[DEPRECATED, must always be true] mirror back side along absolute zero instead of board center")
       ("mirror-axis", po::value<Length>()->default_value(Length(0)), "For two-sided boards, the PCB needs to be flipped along the axis x=VALUE")
       ("mirror-yaxis", po::value<bool>()->default_value(false)->implicit_value(true), "For two-sided boards, the PCB needs to be flipped along the y axis instead");
   add_group("alignment", "always", alignment_options);

   po::options_description cnc_options("CNC options, common to all the milling, drilling, and cutting");
   cnc_options.add_options()
       ("zsafe", po::value<Length>(), "safety height (Z-coordinate during rapid moves)")
       ("spinup-time", po::value<Time>()->default_value(parse_unit<Time>("1 ms")), "time required to the spindle to reach the correct speed")
       ("spindown-time", po::value<Time>(), "time required to the spindle to return to 0 rpm")
       ("zchange", po::value<Length>(), "tool changing height")
       ("zchange-absolute", po::value<bool>()->default_value(false)->implicit_value(true), "use zchange as a machine coordinates height (G53)")
       ("tile-x", po::value<int>()->default_value(1), "number of tiling columns. Default value is 1")
       ("tile-y", po::value<int>()->default_value(1), "number of tiling rows. Default value is 1");
   add_group("cnc", "always", cnc_options);

   cfg_options.add_options()
       ("ignore-warnings", po::value<bool>()->default_value(false)->implicit_value(true), "Ignore warnings")
       ("svg", po::value<string>(), "[DEPRECATED] use --vectorial, SVGs will be generated automatically; this option has no effect")
       ("metric", po::value<bool>()->default_value(false)->implicit_value(true), "use metric units for parameters. does not affect gcode output")
       ("metricoutput", po::value<bool>()->default_value(false)->implicit_value(true), "use metric units for output")
       ("g64", po::value<Length>(), "[DEPRECATED, use tolerance instead] maximum deviation from toolpath, overrides internal calculation")
       ("tolerance", po::value<Length>(), "maximum toolpath tolerance (default is 0.0004in)")
       ("nog64", po::value<bool>()->default_value(false)->implicit_value(true), "do not set an explicit g64")
       ("output-dir", po::value<string>()->default_value(""), "output directory")
       ("basename", po::value<string>(), "prefix for default output file names")
       ("manifest", po::value<string>(), "output file for a JSON list of all the files written, for GUIs and scripts")
       ("preamble-text", po::value<string>(), "preamble text file, inserted at the very beginning as a comment.")
       ("preamble", po::value<string>(), "gcode preamble file, inserted at the very beginning.")
       ("postamble", po::value<string>(), "gcode postamble file, inserted before M9 and M2.")
       ("no-export", po::value<bool>()->default_value(false)->implicit_value(true), "skip the exporting process")
       ("sanity-checks", po::value<bool>()->default_value(false)->implicit_value(true), "run sanity checks")
       ("single-thread", po::value<bool>()->default_value(false)->implicit_value(true), "disable multi-threading");

   // --help shows the command line options, then the generic options that
   // aren't in a group, then the groups.
   std::vector<string> generic;
   for (const auto& name : option_names(cfg_options)) {
     if (std::none_of(groups.cbegin(), groups.cend(), [&](const OptionGroup& group) {
           return std::count(group.options.cbegin(), group.options.cend(), name) > 0;
         })) {
       generic.push_back(name);
     }
   }
   groups.insert(groups.begin(),
                 {{"cli", caption(cli_options), "always", true, option_names(cli_options)},
                  {"generic", caption(cfg_options), "always", false, generic}});
   all_options.add(cli_options).add(cfg_options);
}

void options::add_group(const string& id, const string& used_when,
                        const po::options_description& group) {
  cfg_options.add(group);
  groups.push_back({id, caption(group), used_when, false, option_names(group)});
}

/******************************************************************************/
/*
 */
/******************************************************************************/
// The problems that don't stop pcb2gcode: the message and the options that it's about.
static void warn_about_parameters(
    const OptionCatalog& catalog, po::variables_map const& vm,
    const std::function<void(const char*, std::initializer_list<const char*>)>& warn)
{
    if (vm.count("g64"))
    {
        warn("g64 is deprecated, use tolerance.", {"g64"});
    }

    if (vm.count("tolerance"))
    {
        const double unit = vm["metric"].as<bool>() ? 1. / 25.4 : 1;
        // 0.2mm with --metric, or else 0.008in.
        const double high_tolerance = (vm["metric"].as<bool>() ? 0.2 : 0.008) * unit;
        if (vm["tolerance"].as<Length>().asInch(unit) > high_tolerance)
            warn("Warning: high tolerance value (allowed deviation from toolpath) given.",
                 {"tolerance"});
    }

    if (vm.count("svg")) {
      warn("--svg is deprecated and has no effect anymore, use --vectorial to generate SVGs.",
           {"svg"});
    }

    if (vm.count("drill")
            && !(vm.count("front") || vm.count("back") || vm.count("outline")))
    {
        warn("Warning: Board dimensions unknown. Gcode for drilling will be probably "
             "misaligned.", {"drill", "front", "back", "outline"});
    }

    if (catalog.condition("milling").holds(vm) && vm.count("zwork") &&
        vm["zwork"].as<Length>().asDouble() > 0) {
      warn("Warning: Engraving depth (--zwork) is greater than zero!", {"zwork"});
    }

    if (catalog.condition("cutting").holds(vm) && vm.count("cut-front")) {
      warn("cut-front is deprecated, use cut-side.", {"cut-front"});
    }

    if (catalog.condition("drilling").holds(vm) && vm.count("drill-front")) {
      warn("drill-front is deprecated, use drill-side.", {"drill-front"});
    }
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void options::check_parameters()
{
  po::variables_map const& vm = instance().vm;
  std::vector<OptionError> errors;

  try {
    warn_about_parameters(catalog(), vm, [](const char* what, std::initializer_list<const char*>) {
      cerr << what << "\n";
    });
    check_rules(catalog(), vm, errors);
  } catch (std::runtime_error& re) {
    errors.push_back({string("Invalid parameter: ") + re.what(), ERR_INVALIDPARAMETER});
  }

  if (vm["ignore-warnings"].as<bool>()) {
    std::vector<OptionError> needed;
    for (const auto& error : errors) {
      if (error.needed) {
        needed.push_back(error);
      } else {
        maybe_throw(error.what, error.code);
      }
    }
    errors = needed;
  }
  if (errors.empty()) {
    return;
  }
  string what = error_lines(errors);
  what.pop_back();
  // Scripts and tests rely on the exit code, which is the code of the first
  // problem found.
  throw pcb2gcode_parse_exception(what, errors.front().code);
}

bool options::check_requested(int argc, const char** argv)
{
  // Looked for before parsing, so that a command line that can't be parsed is
  // reported as JSON too.
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--check-options") == 0) {
      return true;
    }
  }
  return false;
}

int options::check_options(int argc, const char** argv, std::ostream& out)
{
  options& self = instance();
  self.vm.clear();
  self.problems.clear();
  self.ignored.clear();
  self.unreadable.clear();
  self.checking = true;
  parse(argc, argv);
  // What a run prints without stopping, in its order.
  std::vector<OptionError> warnings = self.ignored;
  // Nothing is stored when the command line can't be parsed, and then there
  // is nothing more to check.
  if (!self.vm.empty()) {
    std::vector<OptionError> errors;
    warn_about_parameters(catalog(), self.vm,
                          [&](const char* what, std::initializer_list<const char*> names) {
                            warnings.push_back({what, ERR_OK, {names.begin(), names.end()}});
                          });
    check_rules(catalog(), self.vm, errors);
    // Like check_parameters().
    const bool ignoring = self.vm["ignore-warnings"].as<bool>();
    for (const auto& error : errors) {
      // An option whose value can't be read has no value, so its rules would
      // only repeat that problem.
      if (std::none_of(error.options.cbegin(), error.options.cend(),
                       [&](const string& option) { return self.unreadable.count(option) > 0; })) {
        (ignoring && !error.needed ? warnings : self.problems).push_back(error);
      }
    }
  }
  self.checking = false;
  out << check_report(self.problems, warnings) << endl;
  return self.problems.empty() ? ERR_OK : self.problems.front().code;
}

OptionCatalog options::catalog()
{
  return OptionCatalog{instance().all_options, instance().groups, option_conditions(),
                       option_details(), option_rules()};
}

std::string options::dump_options()
{
  return dump_catalog(catalog());
}
