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
deps += dependency('spdlog', version: '>=1.15.3', required: true, include_type: 'system')

# add CLI11 as a dependency
deps += dependency('CLI11', version: '>=2.5.0', required: true, include_type: 'system')

# add zlib as a dependency
z_dep = dependency('zlib', required: true, include_type: 'system')
deps += z_dep

# add zstr as a CMake-based subproject
cmake = import('cmake')
zstr_sub = cmake.subproject('zstr')
deps += zstr_sub.dependency('zstr')

# add Apache Arrow for parquet support
arrow_dep = dependency('arrow', required: true, include_type: 'system')
parquet_dep = dependency('parquet', required: true, include_type: 'system')
deps += [arrow_dep, parquet_dep]

# add Catch2 as a dependency
catch2_dep = dependency(
    'Catch2',
    method: 'cmake',
    version: '>=3.10.0',
    modules: ['Catch2::Catch2WithMain'],
    required: false,
    include_type: 'system',
)
test_deps = deps + catch2_dep

charmvz_lib = library(
    'charmvz_lib',
    [
        'src/sts_parser.cpp',
        'src/rc_parser.cpp',
        'src/log_parser.cpp',
        'src/reconstruction.cpp',
        'src/parquet_writer.cpp',
        'src/builders.cpp',
        'src/schema.cpp',
        'src/utils/log_entry.cpp',
    ],
    dependencies: deps,
    install: true,
)

executable(
    'charmvz',
    'src/main.cpp',
    link_with: charmvz_lib,
    dependencies: deps,
)

# Tests are skipped when Catch2 is not installed, so a plain build never
# requires it.
if catch2_dep.found()
    foreach unit : ['sts_parser', 'parquet_writer', 'log_parser', 'chare_index', 'migration']
        test(
            unit,
            executable(
                'test_' + unit,
                'tests/test_' + unit + '.cpp',
                link_with: charmvz_lib,
                include_directories: include_directories('src', 'tests'),
                dependencies: test_deps,
            ),
        )
    endforeach
endif