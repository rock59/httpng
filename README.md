# HTTPNG module for Tarantool

https://tarantool.io/en/

Based on libh2o from H2O HTTP Server (https://h2o.examp1e.net/)

(well, it would be - when the code is reviewed and accepted
into the "master" branch; unreviewed code is in "dev").

Please note that this prototype uses hardcoded TLS key/certificate file names,
you should either build code in-tree or specify binaries location, e. g.
`LUA_CPATH=build/?.so tarantool examples/basic_get.lua`

API is documented in doc/api.md. Please note that it is not yet finalized.

Architecture is documented in doc/arch.md.

Thank you for your interest in Tarantool!
