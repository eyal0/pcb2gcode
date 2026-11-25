/*
 * This file is part of pcb2gcode.
 * 
 * Copyright (C) 2009, 2010 Patrick Birnzain <pbirnzain@users.sourceforge.net>
 * Copyright (C) 2014, 2015 Nicola Corna <nicola@corna.info>
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

#ifndef OPTIONS_HPP
#define OPTIONS_HPP

#include <stdexcept>

#include <memory>
#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <boost/noncopyable.hpp>

#include <istream>
#include <ostream>
#include <set>
#include <string>
#include <vector>

// Every exit code, as X(name, value).  --dump-options lists them, so add new
// ones here.
#define PCB2GCODE_ERROR_CODES(X) \
    X(ERR_OK, 0) \
    X(ERR_NOZWORK, 1) \
    X(ERR_NOCUTTERDIAMETER, 2) \
    X(ERR_NOZSAFE, 3) \
    X(ERR_NOOFFSET, 4) \
    X(ERR_NOZCUT, 5) \
    X(ERR_NOCUTFEED, 6) \
    X(ERR_NOCUTSPEED, 7) \
    X(ERR_NOCUTINFEED, 8) \
    X(ERR_NOZDRILL, 9) \
    X(ERR_NOZCHANGE, 10) \
    X(ERR_NODRILLFEED, 11) \
    X(ERR_NODRILLSPEED, 12) \
    X(ERR_NOMILLFEED, 13) \
    X(ERR_NOMILLSPEED, 14) \
    X(ERR_ZSAFELOWERZWORK, 15) \
    X(ERR_NEGATIVEMILLFEED, 16) \
    X(ERR_NEGATIVEMILLSPEED, 17) \
    X(ERR_ZSAFELOWERZDRILL, 18) \
    X(ERR_ZSAFELOWERZCHANGE, 19) \
    X(ERR_NEGATIVEDRILLFEED, 20) \
    X(ERR_ZSAFELOWERZCUT, 21) \
    X(ERR_NEGATIVECUTFEED, 22) \
    X(ERR_NEGATIVESPINDLESPEED, 23) \
    X(ERR_LOWCUTINFEED, 24) \
    X(ERR_NOOUTLINEWIDTH, 25) \
    X(ERR_NEGATIVEOUTLINEWIDTH, 26) \
    X(ERR_ZEROOUTLINEWIDTH, 27) \
    X(ERR_NEGATIVEDRILLSPEED, 28) \
    X(ERR_NOSOFTWARE, 29) \
    X(ERR_NOALX, 30) \
    X(ERR_NOALY, 31) \
    X(ERR_NOALPROBEFEED, 32) \
    X(ERR_NEGATIVEBRIDGE, 33) \
    X(ERR_BRIDGENOOPTIMISE, 34) \
    X(ERR_NEGATIVEALX, 35) \
    X(ERR_NEGATIVEALY, 36) \
    X(ERR_NEGATIVEPROBEFEED, 37) \
    X(ERR_NEGATIVECUTVERTFEED, 39) \
    X(ERR_NEGATIVEMILLVERTFEED, 40) \
    X(ERR_NEGATIVETILEX, 41) \
    X(ERR_NEGATIVETILEY, 42) \
    X(ERR_BOTHDRILLFRONTSIDE, 43) \
    X(ERR_UNKNOWNDRILLSIDE, 44) \
    X(ERR_BOTHCUTFRONTSIDE, 45) \
    X(ERR_UNKNOWNCUTSIDE, 46) \
    X(ERR_VORONOINOVECTORIAL, 47) \
    X(ERR_VORONOINOOUTLINE, 48) \
    X(ERR_BOTHTOLERANCEG64, 49) \
    X(ERR_NEGATIVETOLERANCE, 50) \
    X(ERR_NEGATIVEZWORK, 51) \
    X(ERR_NEGATIVESPINUP, 52) \
    X(ERR_NEGATIVESPINDOWN, 53) \
    X(ERR_FALSEMIRRORABSOLUTE, 54) \
    X(ERR_LOWMILLINFEED, 55) \
    X(ERR_SANITY_CHECKS_FAILED, 56) \
    X(ERR_INVALIDMILLDRILLSTEPOVER, 57) \
    X(ERR_LOWMILLDRILLENTRY, 58) \
    X(ERR_MILLDRILLENTRYORDER, 59) \
    X(ERR_INVALIDPARAMETER, 100) \
    X(ERR_UNKNOWNPARAMETER, 101)

enum ErrorCodes {
#define PCB2GCODE_ERROR_CODE(name, value) name = value,
    PCB2GCODE_ERROR_CODES(PCB2GCODE_ERROR_CODE)
#undef PCB2GCODE_ERROR_CODE
};

class pcb2gcode_parse_exception : public std::exception {
 public:
  pcb2gcode_parse_exception(const std::string& what, ErrorCodes error_code) {
    what_string = what;
    this->error_code = error_code;
  }
  virtual const char* what() const throw() {
    return what_string.c_str();
  }
  virtual ErrorCodes code() const throw() {
    return error_code;
  }

 private:
  std::string what_string;
  ErrorCodes error_code;
};

// A problem with the options.  A warning, which doesn't stop pcb2gcode, has
// the code ERR_OK and no rule.
struct OptionError {
  std::string what;
  ErrorCodes code;
  // The options that the problem involves.
  std::vector<std::string> options;
  // The check that failed: the id of a rule in --dump-options, or a check
  // that pcb2gcode makes while reading the options, like "unknown_option".
  std::string rule;
  // The run needs the missing value, so --ignore-warnings can't skip this.
  bool needed = false;
};


// A group of options as --help shows them.
struct OptionGroup {
    std::string id;
    std::string title;
    // The named condition under which pcb2gcode uses the group's options.
    std::string used_when;
    bool cli_only;
    std::vector<std::string> options;
};

struct OptionCatalog;

/******************************************************************************/
/*
 */
/******************************************************************************/
class options : private boost::noncopyable
{

public:
    static void parse(int argc, const char** argv);
    static void parse_files(const std::vector<std::string>& config_files,
                            const bool defaulted);
    static void check_parameters();
    static po::variables_map& get_vm()
    {
        return instance().vm;
    }
    ;
    static std::string help();
    // Every option and what pcb2gcode knows about it, as JSON.
    static std::string dump_options();
    // Whether the command line has --check-options.
    static bool check_requested(int argc, const char** argv);
    // For --check-options: reads and checks the options like a run does, but
    // reports every problem instead of stopping at the first, and reads no
    // input files.  Prints the problems to out as JSON and returns the exit
    // code.
    static int check_options(int argc, const char** argv, std::ostream& out);
    // The options, their groups and the rules that check_parameters() checks.
    static OptionCatalog catalog();

    static void maybe_throw(const std::string& what, ErrorCodes error_code);
private:
    options();
    void add_group(const std::string& id, const std::string& used_when,
                   const po::options_description& group);
    // Passes the problem to maybe_throw(), or adds it to problems while
    // checking.
    static void report(const OptionError& error);
    static void store_readable(const po::parsed_options& parsed, const std::string& context,
                               ErrorCodes code, bool ignorable);
    static void check_config_file(std::istream& stream, const std::string& context);
    po::variables_map vm;
    po::options_description cli_options;      //CLI options
    po::options_description cfg_options;      // all the non-CLI options
    po::options_description all_options;
    std::vector<OptionGroup> groups;          // in --help order
    // While check_options() runs, problems don't throw.  They are added to
    // problems, or to ignored if --ignore-warnings lets a run go on, and the
    // options whose values can't be read are left out of vm and, if a run
    // stops at them, added to unreadable.
    bool checking = false;
    std::vector<OptionError> problems;
    std::vector<OptionError> ignored;
    std::set<std::string> unreadable;
    static options& instance();
};

#endif // OPTIONS_HPP
