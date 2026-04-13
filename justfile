debug_dir := 'builddir-deb'

[private]
default:
	@just --list

setup-debug:
	meson setup {{debug_dir}} --buildtype=debug -Db_lundef=false -Db_sanitize='address,undefined' --reconfigure

compile-debug: setup-debug
	meson compile -C {{debug_dir}}

cleanup-debug:
	meson compile --clean -C {{debug_dir}}
