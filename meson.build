project('CharmVZ', 'cpp',
        default_options: ['warning_level=3', 'cpp_std=c++20'],
       )

spdlog_dep = dependency('spdlog', version: '>=1.15.3')
cli11_dep = dependency('cli11', version: '>=2.5.0')

files = [
  'src/main.cpp',
  'src/reader/reader.cpp',
  ]

executable('charmvz', files, dependencies: [spdlog_dep, cli11_dep])
