# Google Benchmark is pulled in only when the `bench` option is set, so normal
# builds do not need the Conan dependency. The CMakeDeps generator (configured
# in conanfile.py) provides the config package.
find_package(benchmark REQUIRED)
