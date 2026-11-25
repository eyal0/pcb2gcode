#!/usr/bin/env python3
"""End-to-end test for `pcb2gcode --check-options`.

Runs pcb2gcode the way a GUI or a script would and checks that it prints JSON
in the documented format without writing any files, not even the --manifest:
no errors for an example board, and for broken options the same errors and
exit code that a run stops with.  Every rule that it names is either in --dump-options or one of the
checks made while reading the options.

Usage:
  tools/run_check_options_test.py <pcb2gcode_binary>
"""

from __future__ import print_function

import json
import os
import shutil
import subprocess
import sys
import tempfile

EXAMPLE = os.path.join("tests", "data", "gerbv_example", "multivibrator")
READING_RULES = {"unknown_option", "unexpected_argument", "invalid_value", "config_file",
                 "g64_and_tolerance"}


def _run(binary, args, cwd):
    return subprocess.run([binary] + args, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60)


def _check(binary, args, cwd, rule_ids):
    """The report of --check-options, after checking its format and exit code."""
    result = _run(binary, ["--check-options"] + args, cwd)
    report = json.loads(result.stdout.decode("utf-8"))
    assert sorted(report) == ["errors", "format_version", "warnings"], report
    assert report["format_version"] == 1, report
    for error in report["errors"]:
        assert sorted(error) == ["code", "message", "options", "rule"], error
        assert error["rule"] in rule_ids or error["rule"] in READING_RULES, error
    for warning in report["warnings"]:
        # The errors that --ignore-warnings lets a run go on past have a rule and a code.
        assert sorted(warning) in (["message", "options"],
                                   ["code", "message", "options", "rule"]), warning
        assert warning.get("rule", "unknown_option") in rule_ids | READING_RULES, warning
    expected_code = report["errors"][0]["code"] if report["errors"] else 0
    assert result.returncode == expected_code, "exited with {}, report:\n{}".format(
        result.returncode, json.dumps(report, indent=2))
    return report


def _files(directory):
    return sorted(os.path.relpath(os.path.join(root, name), directory)
                  for root, _, names in os.walk(directory) for name in names)


def main(argv):
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    binary = os.path.abspath(argv[0])
    dump = json.loads(_run(binary, ["--dump-options", "--noconfigfile"], ".").stdout)
    rule_ids = {rule["id"] for rule in dump["rules"]}
    with tempfile.TemporaryDirectory() as tmp:
        board = os.path.join(tmp, "board")
        shutil.copytree(EXAMPLE, board, ignore=shutil.ignore_patterns("expected"))
        files = _files(board)

        # The example's millproject has no problems.  Checking it doesn't read
        # the input files or write anything, so an output directory that
        # doesn't exist doesn't matter.
        report = _check(binary, ["--output-dir=" + os.path.join(tmp, "missing")], board,
                        rule_ids)
        assert report == {"format_version": 1, "errors": [], "warnings": []}, report

        # --manifest is read like the other options, but checking never writes
        # the manifest, wherever it would go.  A run with the same options does.
        millproject_path = os.path.join(board, "millproject")
        with open(millproject_path, "a") as millproject:
            millproject.write("manifest=listed.json\n")
        out = os.path.join(tmp, "out")
        os.mkdir(out)
        outside = os.path.join(tmp, "outside.json")
        for args in (["--output-dir=" + out], ["--output-dir=" + out, "--manifest=" + outside]):
            report = _check(binary, args, board, rule_ids)
            assert report == {"format_version": 1, "errors": [], "warnings": []}, report
        report = _check(binary, ["--manifest=a.json", "--manifest=b.json"], board, rule_ids)
        assert [(error["rule"], error["options"]) for error in report["errors"]] == [
            ("invalid_value", ["manifest"])], report
        assert os.listdir(out) == [] and not os.path.exists(outside), os.listdir(out)
        run = _run(binary, ["--output-dir=" + out], board)
        assert run.returncode == 0, run.stderr
        assert os.path.exists(os.path.join(out, "listed.json")), os.listdir(out)

        # Broken values on the command line, on top of the millproject.
        broken = ["--zsafe=-2mm", "--mill-feed=-5", "--zwork=1mm"]
        report = _check(binary, broken, board, rule_ids)
        assert [error["rule"] for error in report["errors"]] == [
            "greater:zsafe,zwork", "range:mill-feed", "greater:zsafe,zcut",
            "greater:zsafe,zdrill"], report
        assert report["errors"][0]["options"] == ["zsafe", "zwork"], report
        assert report["warnings"] == [{"options": ["zwork"], "message":
                                       "Warning: Engraving depth (--zwork) is greater than zero!"}]
        # A run stops with the same errors and exit code.
        run = _run(binary, broken, board)
        assert run.returncode == report["errors"][0]["code"], run.returncode
        assert run.stderr.decode("utf-8").splitlines() == (
            [warning["message"] for warning in report["warnings"]] +
            ["Error: " + error["message"] for error in report["errors"]]), run.stderr

        # A GUI passes --noconfigfile and every option itself.
        report = _check(binary, ["--noconfigfile", "--back=board.gbr", "--foo=1", "--zsafe=x"],
                        board, rule_ids)
        assert [(error["rule"], error["options"]) for error in report["errors"]] == [
            ("unknown_option", ["foo"]), ("invalid_value", ["zsafe"]),
            ("required:zchange", ["zchange"]), ("required:zwork", ["zwork"]),
            ("required:mill-feed", ["mill-feed"]), ("required:mill-speed", ["mill-speed"])], report

        # A command line that can't be parsed is reported as JSON too.
        report = _check(binary, ["--noconfigfile", "--zsafe"], board, rule_ids)
        assert [error["options"] for error in report["errors"]] == [["zsafe"]], report

        # A problem in a configuration file has the configuration file's exit
        # code, and the file's other lines are still read.
        with open(millproject_path) as millproject:
            text = millproject.read()
        assert "zdrill=-1.6mm\n" in text
        with open(millproject_path, "w") as millproject:
            millproject.write(text.replace("zdrill=-1.6mm\n", "zdrill=deep\n"))
        report = _check(binary, [], board, rule_ids)
        assert [(error["rule"], error["options"], error["code"])
                for error in report["errors"]] == [("invalid_value", ["zdrill"], 100)], report
        assert _run(binary, [], board).returncode == 100
        # Options after a [section] have its name in front.
        with open(millproject_path, "a") as millproject:
            millproject.write("[extra]\nzsafe=1mm\n")
        report = _check(binary, [], board, rule_ids)
        assert [(error["rule"], error["options"]) for error in report["errors"]] == [
            ("unknown_option", ["extra.zsafe"]), ("invalid_value", ["zdrill"])], report
        run = _run(binary, [], board)
        assert run.stderr.decode("utf-8").strip() == report["errors"][0]["message"], run.stderr

        # With --ignore-warnings, a run goes on past the values in a millproject
        # that it can't read, printing each one, and stops at a value that it
        # needs.
        fixture = os.path.join("tests", "data", "broken_examples", "unreadable_value")
        ignored_out = os.path.join(tmp, "ignored")
        os.mkdir(ignored_out)
        for args in ([], ["--ignore-warnings"]):
            args = args + ["--output-dir=" + ignored_out]
            report = _check(binary, args, fixture, rule_ids)
            run = _run(binary, args, fixture)
            stderr = run.stderr.decode("utf-8")
            assert run.returncode == report["errors"][0]["code"], (run.returncode, report)
            assert report["errors"][0]["message"] in stderr, stderr
            for warning in report["warnings"]:
                if "code" in warning:
                    assert "Ignoring error code {}: ".format(warning["code"]) in stderr, stderr
                    assert warning["message"] in stderr, stderr
        assert report["errors"][0]["rule"] == "required:mill-feed", report
        assert len(report["warnings"]) == 3, report
        assert os.listdir(ignored_out) == [], os.listdir(ignored_out)

        # The messages quote the command line, which might not be UTF-8.
        if os.name == "posix":
            result = subprocess.run([os.fsencode(binary), b"--check-options", b"--noconfigfile",
                                     b"--zsafe=1mm", b"--zchange=1mm", b"--\xff=1"],
                                    cwd=board, stdout=subprocess.PIPE, timeout=60)
            report = json.loads(result.stdout.decode("utf-8"))
            assert report["errors"][0]["message"] == "Unknown option --\ufffd=1", report

        assert _files(board) == files, "--check-options wrote files: {}".format(
            sorted(set(_files(board)) - set(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
