############################################################################
# Copyright (C) SchedMD LLC.
############################################################################
import inspect
import logging
import os
import pathlib
from py.io import TerminalWriter
import pwd
import pytest
import _pytest
import re
import shutil
import sys

sys.path.append(sys.path[0] + '/lib')
import atf

# Add test description (docstring) as a junit property
def pytest_itemcollected(item):
    node = item.obj
    desc = node.__doc__.strip() if node.__doc__ else node.__name__
    if desc:
        item.user_properties.append(("description", desc))


def pytest_addoption(parser):
    config_group = parser.getgroup('config mode')
    config_group.addoption('--auto-config', action='store_true', help="the slurm configuration will be altered as needed by the test")
    config_group.addoption('--local-config', action='store_false', dest='auto_config', help="the slurm configuration will not be altered")
    parser.addoption('--include-expect', action='store_true', help="include the expect tests")


def pytest_generate_tests(metafunc):
    if "test_id" in metafunc.fixturenames:
        if metafunc.config.getoption('--include-expect'):
            expect_test_dir = str(pathlib.Path(__file__).resolve().parent.parent / 'expect')
            test_ids = sorted([match.group(1) for f in os.listdir(expect_test_dir) if (match := re.search(r'^test([1-9]?[0-9]\.[0-9]*)$', f))])
            metafunc.parametrize("test_id", test_ids)
        else:
            metafunc.parametrize("test_id", [])


def color_log_level(level, **color_kwargs):
    for handler in logging.getLogger().handlers:
        if isinstance(handler, _pytest.logging.LogCaptureHandler):
            formatter = handler.formatter
            if match := formatter.LEVELNAME_FMT_REGEX.search(formatter._fmt):
                levelname_fmt = match.group()
                formatted_levelname = levelname_fmt % {
                    'levelname': logging.getLevelName(level)
                }
                colorized_formatted_levelname = TerminalWriter().markup(
                    formatted_levelname, **color_kwargs
                )
                formatter._level_to_fmt_mapping[level] = formatter.LEVELNAME_FMT_REGEX.sub(
                    colorized_formatted_levelname, formatter._fmt
                )


@pytest.fixture(scope="session", autouse=True)
def session_setup(request):

    # Set the auto-config property from the option
    atf.properties['auto-config'] = request.config.getoption("--auto-config")

    # Set the include-expect property from the option
    atf.properties['include-expect'] = request.config.getoption("--include-expect")

    # Customize logging level colors
    color_log_level(logging.CRITICAL, red=True, bold=True)
    color_log_level(logging.ERROR, red=True)
    color_log_level(logging.WARNING, yellow=True)
    color_log_level(logging.INFO, green=True)
    color_log_level(logging.NOTE, cyan=True)
    color_log_level(logging.DEBUG, blue=True, bold=True)
    color_log_level(logging.TRACE, purple=True, bold=True)


@pytest.fixture(scope="module", autouse=True)
def module_setup(request, tmp_path_factory):

    atf.properties['slurm-started'] = False
    atf.properties['configurations-modified'] = set()
    atf.properties['accounting-database-modified'] = False
    atf.properties['jobs-submitted'] = False
    atf.properties['orig-environment'] = dict(os.environ)
    #print(f"properties = {atf.properties}")

    # Creating a module level tmp_path mimicing what tmp_path does
    name = request.node.name
    name = re.sub(r'[\W]', '_', name)
    name = name[:30]
    atf.module_tmp_path = tmp_path_factory.mktemp(name, numbered=True)

    yield

    # Teardown
    module_teardown()


def module_teardown():

    # Cancel any jobs that were submitted via submit_job
    if atf.properties['jobs-submitted']:
        atf.cancel_all_jobs(quiet=True)

    if atf.properties['auto-config']:

        # Stop slurm if we started it
        if atf.properties['slurm-started'] == True:
            atf.stop_slurm(quiet=True)

        # Restore any backed up configuration files
        for config in set(atf.properties['configurations-modified']):
            atf.restore_config_file(config)

        # Restore the slurm database if modified
        if atf.properties['accounting-database-modified']:
            atf.restore_accounting_database()

    # Restore the prior environment
    os.environ.clear()
    os.environ.update(atf.properties['orig-environment'])


@pytest.fixture(scope="function", autouse=True)
def function_setup(request, monkeypatch, tmp_path):

    # Log function docstring (test description) if present
    if request.function.__doc__ is not None:
        logging.info(request.function.__doc__)

    # Start each test inside the tmp_path
    monkeypatch.chdir(tmp_path)


@pytest.fixture(scope="class", autouse=True)
def class_setup(request):

    # Log class docstring (test description) if present
    if request.cls.__doc__ is not None:
        logging.info(request.cls.__doc__)


def pytest_fixture_setup(fixturedef, request):

    # Log fixture docstring when invoked if present
    if fixturedef.func.__doc__ is not None:
        logging.info(fixturedef.func.__doc__)


def pytest_keyboard_interrupt(excinfo):
    """ Called for keyboard interrupt """
    module_teardown()
