project(
    'CharmVZ',
    'cpp',
    default_options: ['warning_level=3', 'cpp_std=c++20'],
    license: 'MIT',
    license_files: 'LICENSE',
    version: '0.1.0',
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

# add Apache Arrow for parquet support
arrow_dep = dependency('arrow', required: true)
parquet_dep = dependency('parquet', required: true)
deps += [arrow_dep, parquet_dep]

# add Catch2 as a dependency
test_deps = deps + dependency(
    'Catch2',
    method: 'cmake',
    version: '>=3.10.0',
    modules: ['Catch2::Catch2WithMain'],
    required: false,
)

charmvz_lib = library(
    'charmvz_lib',
    [
        'src/reader/reader.cpp',
        'src/writer/writer.cpp',
        'src/timeline/timeline.cpp',
        'src/utils/log_entry.cpp',
        'src/utils/log_reader.cpp',
    ],
    include_directories: include_directories('src/include/'),
    dependencies: deps,
    install: true,
)

executable(
    'charmvz',
    'src/main.cpp',
    link_with: charmvz_lib,
    include_directories: include_directories('src/include/'),
    dependencies: deps,
)

test(
    'reader tests',
    executable(
        'test_reader',
        [
            'tests/test_reader.cpp',
            'src/reader/reader.cpp',
            'src/timeline/timeline.cpp',
            'src/utils/log_entry.cpp',
            'src/utils/log_reader.cpp',
        ],
        dependencies: test_deps,
        link_with: charmvz_lib,
        include_directories: include_directories('src/include/'),
    ),
)
