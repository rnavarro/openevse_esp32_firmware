# Override the prebuilt liblwip.a with a custom rebuild that enables RDNSS
# (CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=2) so the device auto-learns
# IPv6 DNS resolvers from Router Advertisements (RFC 8106).
#
# This prepends custom_libs/ to the library search path so the linker
# finds our RDNSS-enabled liblwip.a before the framework's default one.

from os.path import join

Import("env")

env.Prepend(LIBPATH=[join("$PROJECT_DIR", "custom_libs")])
