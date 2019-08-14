#!/usr/bin/python

from __future__ import print_function
import unittest2
import subprocess
import os
import tempfile
import shutil
import difflib
import filecmp
import sys
import argparse
import re
import collections
import termcolor
import colour_runner.runner
import in_place
import xml.etree.ElementTree

from concurrencytest import ConcurrentTestSuite, fork_for_tests

TestCase = collections.namedtuple("TestCase", ["name", "input_path", "args", "exit_code"])

EXAMPLES_PATH = "testing/gerbv_example"
BROKEN_EXAMPLES_PATH = "testing/broken_examples"
TEST_CASES = ([TestCase(x, os.path.join(EXAMPLES_PATH, x), [], 0)
              for x in [
                  "am-test",
                  "am-test-counterclockwise",
                  "am-test-extended",
                  "am-test-voronoi",
                  "am-test-voronoi-extra-passes",
                  "am-test-voronoi-front",
                  "edge-cuts-inside-cuts",
                  "edge-cuts-broken-loop",
                  "example_board_al_custom",
                  "example_board_al_linuxcnc",
                  "invert_gerbers",
                  "KNoT-Gateway Mini Starter Board",
                  "KNoT_Thing_Starter_Board",
                  "mill_masking",
                  "mill_masking_voronoi",
                  "milldrilldiatest",
                  "multivibrator",
                  "multivibrator-basename",
                  "multivibrator-clockwise",
                  "multivibrator-contentions",
                  "multivibrator-extra-passes",
                  "multivibrator-extra-passes-big",
                  "multivibrator-extra-passes-two-isolators",
                  "multivibrator-extra-passes-two-isolators-tiles",
                  "multivibrator-extra-passes-two-isolators-tiles-al",
                  "multivibrator-extra-passes-voronoi",
                  "multivibrator-identical-isolators",
                  "multivibrator-no-tsp-2opt",
                  "multivibrator-no-zbridges",
                  "multivibrator_no_export",
                  "multivibrator_no_export_milldrill",
                  "multivibrator_no_zero_start",
                  "multivibrator_pre_post_milling_gcode",
                  "multivibrator-two-isolators",
                  "multivibrator_xy_offset",
                  "multivibrator_xy_offset_zero_start",
                  "multi_outline",
                  "sharp_corner",
                  "sharp_corner_2",
                  "silk",
                  "silk-lines",
                  "slots-milldrill",
                  "slots-with-drill",
                  "slots-with-drill-and-milldrill",
                  "slots-with-drill-metric",
                  "slots-with-drills-available",
              ]] +
              [TestCase("multivibrator_bad_" + x,
                        os.path.join(EXAMPLES_PATH, "multivibrator"),
                        ["--" + x + "=non_existant_file"], 100)
               for x in ("front", "back", "outline", "drill")] +
              [TestCase("broken_" + x,
                        os.path.join(BROKEN_EXAMPLES_PATH, x),
                        [], 100)
               for x in ("invalid-config",)
              ] +
              [TestCase("version",
                        os.path.join(EXAMPLES_PATH),
                        ["--version"],
                        0)] +
              [TestCase("help",
                        os.path.join(EXAMPLES_PATH),
                        ["--help"],
                        0)] +
              [TestCase("tsp_2opt_with_millfeedirection",
                        os.path.join(EXAMPLES_PATH, "am-test"),
                        ["--tsp-2opt", "--mill-feed-direction=climb"],
                        100)] +
              [TestCase("ignore warnings",
                        os.path.join(BROKEN_EXAMPLES_PATH, "invalid-config"),
                        ["--ignore-warnings"],
                        0)] +
              [TestCase("invalid_millfeedirection",
                        os.path.join(EXAMPLES_PATH),
                        ["--mill-feed-direction=invalid_value"],
                        101)]
)

def colored(text, **color):
  if hasattr(sys.stderr, "isatty") and sys.stderr.isatty():
    return termcolor.colored(text, **color)
  else:
    return text

class IntegrationTests(unittest2.TestCase):
  def rotate_pathstring(self, pathstring):
    """Parse a string representing an SVG path.

    Parse it into an array of array of points.  Each array is a series of line
    segments.  They are drawn in order and the evenodd rule determines which is
    the hole and which is the solid part.

    It's intentionally not very flexible in what it supports, in case we
    accidentally parse something that we've never seen before and do it
    incorrectly.
    """
    def string_to_paths(pathstring):
      """Returns an array of paths where each path starts with an absolute move
      (M) and the rest are absolute lineTo (L) until the next moveTo (M).
      """
      pathstring = pathstring[2:-3] # Remove the first M and the final z
      paths = [[tuple(point.split(",")) for point in p.strip().split(" L ")] for p in pathstring.split("M ")]
      # Now paths is a list.  Each element is an array of points.  Each point is a pair of strings, x,y.
      return paths

    def rotate_path(path):
      """Rotate the path so that the least element is first.

      Only rotate is the first and last element match.
      """
      self.assertEqual(path[0], path[-1])
      index_of_smallest = min(enumerate(path),
                              key=lambda x: (float(x[1][0]), float(x[1][1]), x[0]))[0]
      rotated_points = path[index_of_smallest:-1] + path[:index_of_smallest+1]
      return rotated_points

    def paths_to_string(paths):
      """Return the path string that represents these paths."""
      return "M " + "M ".join(" L ".join(','.join(point) for point in path) for path in paths) + " z "
    self.assertEqual(pathstring, paths_to_string(string_to_paths(pathstring)))
    return paths_to_string(rotate_path(p) for p in string_to_paths(pathstring))

  def fix_up_expected(self, path):
    """Fix up any files made in the output directory

    This will enlarge all SVG by a factor of 10 in each direction until they are
    at least 1000 in each dimension.  This makes them easier to view on github.

    Also adjust the order of all SVG paths that start and end at the same place
    to start on the smallest element.  This will make github diffs smaller in
    some cases where a no-effect union or intersection of polygons chnaged the
    order of points.
    """
    def bigger(matchobj):
      width = float(matchobj.group('width'))
      height = float(matchobj.group('height'))
      while width < 1000 or height < 1000:
        width *= 10
        height *= 10
      return 'width="' + str(width) + '" height="' + str(height) + '" '
    for root, subdirs, files in os.walk(path):
      for current_file in files:
        with in_place.InPlace(os.path.join(root, current_file)) as f:
          for line in f:
            if line.startswith("<svg"):
              f.write("<!-- original:\n" +
                      line +
                      "-->\n" +
                      re.sub('width="(?P<width>[^"]*)" height="(?P<height>[^"]*)" ',
                             bigger,
                             line))
            elif line.startswith('<g fill-rule="evenodd"><path d="M '):
              etree = xml.etree.ElementTree.fromstring(line)
              pathstring = etree[0].attrib['d']
              f.write(line.replace(pathstring, self.rotate_pathstring(pathstring)))
            else:
              f.write(line)

  def pcb2gcode_one_directory(self, input_path, cwd, args=[], exit_code=0):
    """Run pcb2gcode once in one directory.

    Current working directory remains unchanged at the end.

    input_path: Where to run pcb2gcode
    Returns the path to the output files created.
    """
    pcb2gcode = os.path.join(cwd, "pcb2gcode")
    actual_output_path = tempfile.mkdtemp()
    os.chdir(input_path)
    try:
      cmd = [pcb2gcode, "--output-dir", actual_output_path] + args
      print("Running {}".format(" ".join("'{}'".format(x) for x in cmd)))
      p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
      result = p.communicate()
      self.assertEqual(p.returncode, exit_code)
      self.fix_up_expected(actual_output_path)
    finally:
      print(result[0], file=sys.stderr)
      os.chdir(cwd)
    return actual_output_path

  def compare_directories(self, left, right, left_prefix="", right_prefix=""):
    """Compares two directories.

    Returns a string representing the diff between them if there is
    any difference between them.  If there is no difference between
    them, returns an empty string.
    left: Path to left side of diff
    right: Path to right side of diff
    left_prefix: String to prepend to all left-side files
    right_prefix: String to prepend to all right-side files
    """

    # Right side might not exist.
    if not os.path.exists(right):
      all_diffs = []
      for f in os.listdir(left):
        all_diffs += "Found %s but not %s.\n" % (os.path.join(left_prefix, f), os.path.join(right_prefix, f))
        left_file = os.path.join(left, f)
        with open(left_file, 'r') as myfile:
          data=myfile.readlines()
          all_diffs += difflib.unified_diff(data, [], '"' + os.path.join(left_prefix, f) + '"', "/dev/null")
      return ''.join(all_diffs)

    # Left side might not exist.
    if not os.path.exists(left):
      all_diffs = []
      for f in os.listdir(right):
        all_diffs += "Found %s but not %s.\n" % (os.path.join(right_prefix, f), os.path.join(left_prefix, f))
        right_file = os.path.join(right, f)
        with open(right_file, 'r') as myfile:
          data=myfile.readlines()
          all_diffs += difflib.unified_diff([], data, "/dev/null", '"' + os.path.join(right_prefix, f) + '"')
      return ''.join(all_diffs)


    diff = filecmp.dircmp(left, right)
    # Now compare all the differing files.
    all_diffs = []
    for f in diff.left_only:
      all_diffs += "Found %s but not %s.\n" % (os.path.join(left_prefix, f), os.path.join(right_prefix, f))
      left_file = os.path.join(left, f)
      with open(left_file, 'r') as myfile:
        data=myfile.readlines()
        all_diffs += difflib.unified_diff(data, [], '"' + os.path.join(left_prefix, f) + '"', "/dev/null")
    for f in diff.right_only:
      all_diffs += "Found %s but not %s.\n" % (os.path.join(right_prefix, f), os.path.join(left_prefix, f))
      right_file = os.path.join(right, f)
      with open(right_file, 'r') as myfile:
        data=myfile.readlines()
        all_diffs += difflib.unified_diff([], data, "/dev/null", '"' + os.path.join(right_prefix, f) + '"')
    for f in diff.diff_files:
      left_file = os.path.join(left, f)
      right_file = os.path.join(right, f)
      with open(left_file, 'r') as myfile0, open(right_file, 'r') as myfile1:
        data0=myfile0.readlines()
        data1=myfile1.readlines()
        all_diffs += difflib.unified_diff(data0, data1, '"' + os.path.join(left_prefix, f) + '"', '"' + os.path.join(right_prefix, f) + '"')
    return ''.join(all_diffs)

  def run_one_directory(self, input_path, cwd, expected_output_path, test_prefix, args=[], exit_code=0):
    """Run pcb2gcode on a directory and return the diff as a string.

    Returns an empty string if there is no mismatch.
    Returns the diff if there is a mismatch.
    input_path: Path to inputs
    expected_output_path: Path to expected outputs
    test_prefix: Strin to prepend to all filenamess
    """
    actual_output_path = self.pcb2gcode_one_directory(input_path, cwd, args, exit_code)
    if exit_code:
      return ""
    diff_text = self.compare_directories(expected_output_path, actual_output_path,
                                         os.path.join("expected", test_prefix),
                                         os.path.join("actual", test_prefix))
    shutil.rmtree(actual_output_path)
    return diff_text

  def do_test_one(self, test_case, cwd):
    test_prefix = os.path.join(test_case.input_path, "expected")
    input_path = os.path.join(cwd, test_case.input_path)
    expected_output_path = os.path.join(cwd, test_case.input_path, "expected")
    print(colored("\nRunning test case:\n" + "\n".join("    %s=%s" % (k,v) for k,v in test_case._asdict().items()), attrs=["bold"]), file=sys.stderr)
    diff_text = self.run_one_directory(input_path, cwd, expected_output_path, test_prefix, test_case.args, test_case.exit_code)
    self.assertFalse(bool(diff_text), 'Files don\'t match\n' + diff_text)

if __name__ == '__main__':
  parser = argparse.ArgumentParser(description='Integration test of pcb2gcode.')
  parser.add_argument('--fix', action='store_true', dest='fix',
                      help='Update expected outputs automatically')
  parser.add_argument('--no-fix', action='store_false', dest='fix',
                      help='Don\'t update expected outputs automatically')
  parser.add_argument('--add', action='store_true', default=False,
                      help='git add new expected outputs automatically')
  parser.add_argument('-j', type=int, default=3,
                      help='number of threads for running tests concurrently')
  parser.add_argument('--tests', type=str, default="",
                      help='regex of tests to run')
  args = parser.parse_args()
  if args.tests:
    TEST_CASES = [t for t in TEST_CASES if re.search(args.tests, t.name)]
  cwd = os.getcwd()
  def add_test_case(t):
    def test_method(self):
      self.do_test_one(t, cwd)
    setattr(IntegrationTests, 'test_' + t.name, test_method)
    test_method.__name__ = 'test_' + t.name
    test_method.__doc__ = str(test_case)
  for test_case in TEST_CASES:
    add_test_case(test_case)
  if args.fix:
    print("Generating expected outputs...")
    output = None
    try:
      subprocess.check_output(sys.argv + ['--no-fix'], stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError, e:
      output = str(e.output)
    if not output:
      print("No diffs, nothing to do.")
      exit(0)
    p = subprocess.Popen(["patch", "-p1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    result = p.communicate(input=output)
    files_patched = []
    for l in result[0].split('\n'):
      if l.startswith("patching file '"):
        files_patched.append(l[len("patching file '"):-1])
      elif l.startswith("patching file "):
        files_patched.append(l[len("patching file "):])
    print(result[0])
    if args.add:
      subprocess.call(["git", "add"] + files_patched)
      print("Done.\nAdded to git:\n" +
            '\n'.join(files_patched))
    else:
      print("Done.\nYou now need to run:\n" +
            '\n'.join('git add ' + x for x in files_patched))
  else:
    test_loader = unittest2.TestLoader()
    all_test_names = ["test_" + t.name for t in TEST_CASES]
    test_loader.sortTestMethodsUsing = lambda x,y: cmp(all_test_names.index(x), all_test_names.index(y))
    suite = test_loader.loadTestsFromTestCase(IntegrationTests)
    if args.j > 1:
      suite = ConcurrentTestSuite(suite, fork_for_tests(args.j))
    if hasattr(sys.stderr, "isatty") and sys.stderr.isatty():
      test_result = colour_runner.runner.ColourTextTestRunner(verbosity=2).run(suite)
    else:
      test_result = unittest2.TextTestRunner(verbosity=2).run(suite)
    if not test_result.wasSuccessful():
      print('\n***\nRun one of these:\n' +
            './integration_tests.py --fix\n' +
            './integration_tests.py --fix --add\n' +
            '***\n')
      exit(1)
    else:
      exit(0)
