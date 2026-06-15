#!/bin/sh
# Build entry point. Wraps the Makefile with readable output.
#
#   ./build.sh            build both binaries (default)
#   ./build.sh gui        build the GUI only
#   ./build.sh cli        build the CLI only
#   ./build.sh run        build and launch the GUI
#   ./build.sh run-cli    build and launch the CLI
#   ./build.sh test       build and run the tests
#   ./build.sh clean      remove build/

set -e
cmd="${1:-all}"

case "$cmd" in
all)
	echo "==> building tune_queue_gui and tune_queue_cli"
	make all
	;;
gui)
	echo "==> building tune_queue_gui"
	make gui
	;;
cli)
	echo "==> building tune_queue_cli"
	make cli
	;;
run)
	echo "==> launching the GUI"
	make run
	;;
run-cli)
	echo "==> launching the CLI"
	make run-cli
	;;
test)
	echo "==> building and running the tests"
	make test
	;;
clean)
	echo "==> cleaning"
	make clean
	;;
*)
	echo "usage: $0 [all|gui|cli|run|run-cli|test|clean]" >&2
	exit 1
	;;
esac
