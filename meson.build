project(
  'CharmVZ',
  'cpp',
  default_options: ['warning_level=3', 'cpp_std=c++20'],
)
deps = []
# add spdlog as a dependency
deps += dependency('spdlog', version: '>=1.15.3', required: true)

# add CLI11 as a dependency
deps += dependency('CLI11', version: '>=2.5.0', required: true)

# add zlib as a dependency
z_dep = dependency('zlib', required: true)
deps += z_dep

# add zstr as a CMake-based subproject
cmake = import('cmake')
zstr_sub = cmake.subproject('zstr')
deps += zstr_sub.dependency('zstr')

# add Catch2 as a dependency
test_deps = deps + dependency(
  'Catch2',
  method: 'cmake',
  version: '>=3.10.0',
  modules: ['Catch2::Catch2WithMain'],
  required: false,
)

files = [
  'src/main.cpp',
  'src/reader/reader.cpp',
]

executable('charmvz', files, dependencies: deps)

# add tests
test_files = [
  'tests/test_reader.cpp',
]

test(
  'reader tests',
  executable('test_reader', test_files, dependencies: test_deps),
)