cmake_minimum_required(VERSION 3.16)

# Unit tests for main/auth.c against docs/test-vectors.json.
#   cd firmware/test_apps/auth_test
#   idf.py set-target esp32s3 && idf.py build flash monitor
#
# auth.c/canon_json.c/b64url.c are pulled in directly from ../../main so the
# firmware and the tests can never drift apart.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(rw_auth_test)
